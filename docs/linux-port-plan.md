# Rufus Linux port implementation plan

Updated: 2026-10-18

**Release priority decision (2026-10-24):** the user deferred B6 UDF 2.50/2.60
metadata-partition work and retained existing UDF 1.02–2.01 support. Focus on
finishing a useful release with disclosed gaps. This supersedes B6 scheduling
in the older full-parity plan below. The user then removed only ReFS formatting
from first-release requirements. The user subsequently also deferred Dev Drive detection. CP77 qualifies the
supported-UDF saved choice and optional provider diagnostic; the actual UDF
attribute remains unavailable. CP78 reuses the original 38-language catalog with user-accepted Linux-specific English fallback; full new translations remain a follow-up. Release evidence, desktop and package qualification and GitHub/AUR delivery proceed in B8. See the [current release plan](linux-release-plan.md#useful-release-review--2026-10-24)
and [documented UDF gap](linux-filesystem-formatting.md#deferred-udf-250260-gap--user-decision-2026-10-24).
Do not claim full Windows parity or resume either deferred formatter implicitly.

**Host settings decision (2026-09-29):** do not add more host detection to infer
user settings, even when Windows Rufus uses it. Offer explicit choices from
Rufus's existing lists. Existing detection may stay; extending it is not a
priority. Target/image validation and required native-tool checks still apply.

**Consolidation completed (2026-09-27).** The [production consolidation pass](linux-consolidation.md)
is complete; CP17d5d now integrates shared HDD/card filtering and protected fixed-media
acquisition. CP17e1 qualifies NTFS-3G block mounts through the existing native
unmount path; CP17e2 will qualify a complete helper job starting from one. This supersedes
older requirements in this checkpoint history to retain helper-independent
unmounted DD: one helper now owns authorization, acquisition, writes and cleanup
for every DD job. Keep the existing ISO/DD choice, percentage calculation and
native DD sector support. A missing matching helper gates all creation modes.

## Objective

Build a Linux-native Rufus application with a compact Qt6 Widgets interface,
preserving Rufus's image recognition, media preparation rules and boot support.
Reuse original Rufus source files wherever practical, and use established Linux
libraries, services and utilities for the operations currently implemented with
Windows APIs. Write new code for the GUI, integration and genuinely missing
platform behavior.

Develop in the [mcirsta/rufus fork](https://github.com/mcirsta/rufus). Structure
the work so it can eventually be proposed upstream with a small maintenance
burden. Upstream acceptance is a future maintainer decision; the Linux port
must remain maintainable and buildable independently.

This is the current implementation plan. Earlier Lufus planning documents and
the reuse assessment remain background material; this document governs the
frontend choice, reporting integration and implementation sequence.
Use the [testing and checkpoint plan](testing-and-checkpoints.md) to select
the next bounded change and its required evidence. The phases below describe
the overall scope; they are not single implementation steps.

**Current priority, revised 2026-09-16:** the narrow raw-image workflow has
passed virtual USB and BIOS/UEFI boot qualification. Build the native ISO-mode
path in small tested steps, keeping Rufus's ISO default and selectable DD mode.
CP10c supplies ordinary-file extraction into disposable directories; CP10d
formats a FAT32 region in disposable image files using native tools. CP10e adds
a GPT main partition using common Rufus placement arithmetic and native libfdisk,
with tests formatting its actual extent. CP10f qualifies ordinary extraction on
mounted FAT32 in a disposable Linux USB guest. CP10g shares Rufus's filename
substitutions between Windows and Linux, including nested output directories
and cleanup. CP10h follows Rufus's ordinary-file replacement and directory reuse
when output names collide. CP10i qualifies GPT creation on retained guest block
descriptors. CP10j qualifies releasing our claim, running stock mkfs.fat on the
partition, then reacquiring access and checking the result. CP10k runs those
guest operations through one real polkit authorization from an unprivileged Qt
caller, with approval caching disabled. CP10l adds the helper process and Qt
controller for one GPT/FAT32 image-file job, including preflight, phase/results,
EOF cancellation and child cleanup. CP10m adds the explicit native-device request,
root-side target revalidation and one polkit launch to that connection. CP10n adds
source preflight, private mounting, tree extraction and flush/unmount cleanup in
that same authorized job. CP10o boots a generated, self-contained EFI ISO tree
through that helper's GPT/FAT32 USB output. CP10p adds an official Alpine standard
ISO and validates its unchanged EFI boot files through that same path. CP10q shares
root Syslinux configuration generation with Windows and applies it after Linux
extraction. CP10r shares the token matching loop and adds a tested Linux text-file
adapter. CP10s shares the ordered label/Red Hat passes and space escaping, applies
them during extraction in the same helper, and validates text and resulting sizes
before GPT writes. CP10t adds shared FreeNAS volume-reference and EFI Syslinux
config-copy rules. CP10u adds a read-only GPT/FAT32 image/settings preflight,
reusing the source, geometry, formatter, policy and extraction checks. It inspects
the actual x64 fallback EFI application header and preserves unresolved Secure
Boot facts and all original mode choices. CP10v connects that preflight to the
confirmed Widgets job and single authorized helper, including explicit hybrid DD
handoff. Its acceptance test drives START with live device discovery and boots the
resulting USB in a separate VM; see the checkpoint record for results. Header/tree checks establish
prerequisites, not arbitrary payload bootability or complete allocation fit.
Further image-analysis variants,
including standalone WIM/ESD SELECT integration, stay deferred. Completion is
measured by supported workflows.

## Full parity scope (2026-09-25)

Implement the complete feature set of Windows Rufus, including SFD/superfloppy,
advanced and legacy features. There is no required feature order. Select bounded
checkpoints by dependencies and usefulness; being uncommon or old is not grounds
for excluding a feature. The [feature parity backlog](linux-feature-parity.md)
tracks the work groups, Windows source references, existing Linux decisions and
acceptance rules. Historical deferrals below describe sequencing, not removals
from the final scope. Keep the single-customization test limit unchanged.

## Agreed decisions

| Area | Decision |
| --- | --- |
| Frontend | Qt6 Widgets and C++; a small desktop application using standard controls and dialogs. |
| UI structure | Follow the Win32 GUI exactly in layout, labels, control order and default visibility, using `src/rufus.rc` and `src/ui.c` as the source of truth. Preserve keyboard navigation and scalable sizing with Qt Widgets. |
| Lufus reuse | Treat Lufus as a workflow reference; the original Rufus Win32 GUI governs the new form. Replace Lufus's worker, global logger, device handling and custom formatter. |
| Rufus reuse | Prefer compiling the existing C files directly from `src/`, with alternate platform implementations selected by the Linux build. |
| Reporting | Implement existing C reporting entry points in a small C++ adapter and expose typed Qt signals. Use queued delivery to the GUI. |
| Linux operations | Prefer existing libraries and services; use established command-line tools where appropriate. Add custom platform code only for a concrete gap. |
| Windows impact | Keep the existing Windows implementation and build independent of Linux and Qt. Avoid scattered platform conditionals and broad refactoring. |
| Shared media logic | Separate media rules from GUI code wherever they are mixed. Make focused upstream-visible changes, with both frontends calling the same C implementation. |
| Behavior differences | Preserve Rufus behavior by default. A small divergence needs a strong, concrete reason and a coherent user-visible result; implementation convenience is insufficient. Document and test the original behavior, difference and justification. |
| Adapters | Use small Windows-to-Linux API adapters and Linux implementations of existing Rufus interfaces where the semantics map cleanly. They adapt platform operations; they do not emulate GUI dependencies. Wine/Winelib are excluded. |
| Build | Linux is strictly CMake-based, with its entry point under `linux/` and Ninja's default parallelism. Windows retains Rufus's existing Visual Studio/MSBuild and MinGW/Autotools build paths. |
| Execution | Keep the GUI unprivileged and responsive. Serialize jobs that use Rufus's process-global state. |
| Privileged work | At most one native authorization request per USB creation; one helper retains privileges through all phases and cleanup. The Qt GUI stays unprivileged. |
| Coding style | Follow the direct style of `allparts_new/src`: named functions, concrete data, explicit control flow, no lambdas, and templates only where their benefit is substantial. |
| Performance | Keep disk and image loops efficient, bound notification work, and measure representative workloads. |
| Delivery | Implement and validate useful media paths incrementally. Do not claim full Rufus feature parity from the initial releases. |
| Checkpoints and tests | Work on one bounded checkpoint at a time, with explicit acceptance tests. Use simulated devices, temporary files and QEMU USB emulation while physical USB media is unavailable. |
| ISOHybrid mode choice | Keep Rufus's existing dialog, ISO-mode recommendation/default and selectable DD mode. Preserve its DD-required exceptions and conditional choices. Do not introduce a Linux-specific or filename-based distro default. |

Initial ISO integration boundary (CP10u/CP10v): ordinary GPT main partition, FAT32,
512-byte logical sectors, an explicitly validated cluster size and valid
FAT label, no persistence, and a regular x64 fallback EFI application in the
selected image tree. Reuse the image's EFI executable and payload. A Syslinux
configuration or unknown BIOS installer version does not by itself require a
native Syslinux installation for this UEFI path. Keep BIOS and other architecture
support separate until qualified. Reject incomplete content/tree analysis,
Windows media, required NTFS/large-file handling, incompatible GRUB filesystem
support and Rufus's existing ISO-disabled cases. Apply the existing extractor's
complete-tree/config validation with the FAT32 file-size limit.

Secure Boot checks remain pending; preflight must not relabel them complete.
Likewise, an unchecked embedded EFI image can remain pending when a usable
ordinary fallback candidate is supplied by the selected tree. These facts are
separate from native write readiness. The preflight's header check is structural;
it does not validate executable sections, trust, configuration dependencies or
bootability. Keep the existing representative USB boot checks as required
evidence when enabling the GUI workflow, with no image filename/hash whitelist.

START retains the accepted source/settings, checks the live target, preserves
the default ISO choice, confirms destruction and uses one preparation helper
through cleanup. The existing DD plan explicitly accepts the analyzed hybrid
source and checks that identity again; ordinary raw selection still requires `.img`.
Rufus also offers ISO → ESP for small GPT/FAT hybrids (including the Alpine test
image). That row remains visible but disabled with an explanation until its Linux
implementation is ready; it must never execute as ordinary ISO mode or DD. Full allocation accounting
and distribution-specific payload dependencies remain outside the read-only
prerequisite check and must not be advertised as proven by it.

CP10w extracts Rufus's volume-label normalization and FAT32 cluster-size policy
into `src/format_policy.c`, called by both native adapters. ISO confirmation shows
the normalized label that the helper will write. The form uses Rufus's allowed
sizes and default, filtered by the actual GPT partition geometry and the native
formatter's supported range. DD remains independent of formatting settings.
The native formatter still rejects edge spaces and control characters left by
the original normalizer; extending that support is unfinished parity work.
Extended label/icon creation remains visibly unavailable rather than claiming a
setting the helper does not apply. Keep further storage layouts and installers
out of this bounded usability checkpoint.

**Milestone, agreed 2026-09-16: Windows 11 installation media.**
Continue through small tested steps toward an ordinary installer before adding
Windows customization options. After CP10w's label/cluster behavior:

1. Trace Rufus's Windows filesystem and boot-layout decisions and identify the
   smallest shared extraction points. Follow its NTFS/UEFI:NTFS route where that
   is the selected behavior; qualify WIM splitting when supporting its FAT32 path.
2. Add the required native formatting and boot-payload handling within the same
   authorized helper. Test ordinary files and disposable USB devices before
   exposing new settings. Preserve the one-prompt requirement through cleanup.
3. Connect Windows ISO preparation using shared Rufus rules and upstream wimlib
   where needed. Validate large files, output metadata and failure/cancellation.
4. Create Windows 11 installation media in the USB simulator, independently
   inspect the output, boot Windows Setup and install onto a disposable VM disk.
   Merely reaching an EFI executable or parsing a WIM is not this milestone.
5. Add Rufus's Windows customization options after the ordinary installer.
   Per the scope decision on 2026-09-17, only one representative customization
   qualification is in scope. CP11n's privacy-only media creation/readback passes
   and fulfills that scope. Installed-Windows customization behavior, testing
   every option and a Windows-version matrix remain deferred.

BIOS installers, extra Linux distribution workarounds, persistence and unrelated
filesystems should not displace this milestone unless they are concrete prerequisites.

CP11a traces those Windows decisions in [the installer implementation notes](linux-windows-installer.md)
and shares Rufus's trailing-partition arithmetic. The native GPT writer now
supports the ordinary main partition plus the UEFI:NTFS Basic Data partition on
regular files, including its no-drive-letter attribute. This is table creation
only. CP11b adds native NTFS formatting on partition-sized regular files, reusing
Rufus's cluster-size rules and the FAT formatter's child supervision. CP11c writes
the unchanged bundled boot payload through the bounded raw writer and qualifies
x64 OVMF → UEFI:NTFS → NTFS EFI/Linux boot with independent file checks.
CP11d adds a preparation-only NTFS request to that same authorized helper: create
both GPT partitions, check both kernel extents and format the main partition.
Unlike mkfs.fat, the qualified mkntfs permits retaining the whole-disk claim
throughout formatting and readback. CP11e embeds the unchanged UEFI:NTFS image in
the helper and writes/verifies it through the same retained whole-disk descriptor.
The bounded raw writer preserves the main filesystem, GPT and boot-partition
padding. CP11f adds NTFS mount/extraction/cleanup to the same helper and shared
extractor. Prefer ntfsprogs-plus and the new kernel `ntfs` driver; use NTFS-3G
when that kernel support is unavailable. Keep its FUSE process under the helper
until unmount completes, with the same single authorization. Driver errors do
not trigger an automatic retry with another provider.

CP11g adds complete ISO9660/UDF extraction fixtures with a file beyond 4 GiB.
Both formats pass creation and independent readback with the new kernel driver
and NTFS-3G, including sanitizer-built fallback jobs. CP11j connects this helper
path to shared Windows ISO selection and preflight in Widgets.
Keep each stage independently testable in the USB simulator and keep one
authorization through the complete job. Prefer host KVM for these large fixtures;
check its availability in the environment that launches QEMU, since a tool
sandbox may hide `/dev/kvm`.

CP11h extracts filesystem eligibility and preference from the Win32 controls
into the existing `src/format_policy.c`. Windows uses the shared rules and
retains its dropdown updates and warning; Linux's current FAT32 preflight uses
the same compatibility check. The shared enums retain their original values.
CP11i also shares `FormatThread`'s extra-partition decision in this module,
preserving the original UEFI:NTFS, ESP/MSR, persistence and old-BIOS flags.
CP11j applies these rules to Widgets and read-only ISO preflight, intersecting
filesystem eligibility with native formatter and GPT-layout capabilities.
The form offers FAT32/NTFS and Rufus's preferred filesystem, shared label
normalization and supported cluster sizes. NTFS selects the same helper's
UEFI:NTFS route, including large installation files without WIM splitting.
Preflight rereads Windows metadata from the retained ISO/UDF source and accepts
known Windows 10/11 installation images with a regular x64 UEFI fallback;
older, unknown or unreadable Windows images remain unsupported. Confirmation
and execution retain one helper and one authorization through cleanup.
Generated metadata images and simulated devices cover this connection; they
do not establish that Windows Setup boots or installs.

CP11k completes the ordinary Windows 11 installation-media milestone in the
USB simulator. The supplied `Win11_25H2_English_x64_v2.iso` passes actual Widgets
creation with one native authorization, ntfsprogs-plus and the new kernel `ntfs`
driver. Independent UDF/NTFS3 readback matches all 976 files and 94 directories,
including the 7.6 GB installation WIM; GPT, NTFS and the unchanged UEFI:NTFS payload
also pass. Windows 11 Pro 25H2 installs onto a separate empty VM disk and boots
to its first-use region screen with the USB detached. The installed system
reports build 26200.8037 from `C:\WINDOWS`. Account setup and activation are not
part of this baseline result. The fixture uses KVM, a private TPM 2.0 and Secure
Boot-capable OVMF with enforcement disabled; physical USB and Secure Boot
qualification remain separate. See the CP11k testing record for evidence.

CP11l extracts Rufus's existing answer-file writer into shared C. Windows still
gathers its own host settings and applies the result; Linux builds and tests the
same writer without Win32 dependencies. Output, removal offsets and username
normalization match the original caller in 65,536 comparisons. Allocation and
stream failures are reported instead of accepting a partial answer file.

CP11m connects the first native application path to the authorized helper:
online-account requirement, local account, privacy, BitLocker and QoL choices
use the shared answer file at `sources/$OEM$/$$/Panther/unattend.xml`. This is
Rufus's original path for options without a WindowsPE pass. Shared eligibility
rules and helper preflight reject unsupported image/choice combinations before
partition writes. Generated ISO/UDF extraction and answer-file tests pass.

CP11n connects those five options through a Qt Widgets Windows User Experience
dialog after image preflight and before destructive confirmation/authorization.
It uses the original option order, wording, version eligibility, supported
defaults, username normalization and remembered selection mask. Cancel and device
removal close the dialog without starting the helper. Eligible deferred options
are disabled and explained; they are never sent as selected options.

CP11n initially kept the disabled silent-install row visible to explain its
pending Linux support. CP11aa restores Win32's prerequisite-driven visibility:
local-account and privacy choices plus explicitly accepted regional settings
reveal it. The permanently disabled Windows-host regional-copy checkbox is not
a prerequisite. A button beside it opens the explicit editor. Edition selection
and Rufus's original three acknowledgements precede USB confirmation/authorization.

The real Windows 11 ISO passes creation and independent readback in the USB
simulator with only **Skip privacy questions** selected and one authorization.
All original files match; only the expected OEM answer file and directories are
added. This completes the single customization qualification in scope. Its
effect inside installed Windows has not been tested and remains deferred.

CP11o adds the native `boot.wim` update adapter, directly using upstream
wimlib's update/overwrite APIs. It preserves Rufus's Setup-image selection and
compression policy, handles cancellation and reports flush failures. Small
generated WIM component tests pass normally and with sanitizers. CP11q connects
this adapter to the helper and enables the hardware-requirements option.

CP11p adds native offline SYSTEM-hive editing through the distribution Hivex
library. It reuses Rufus's shared Setup/LabConfig value names, preserves unrelated
entries and writes a separate output, leaving the input untouched. Small fixture
tests include independent wimlib readback and cancellation/failure cleanup.

CP11q integrates the hardware-requirements path: offline SYSTEM editing with
Rufus's answer-file fallback, the appraiser placeholder, and the unchanged
bundled x64/ARM64 setup wrappers for builds 26000 and later. The helper initializes
wimlib with case-insensitive lookup before its first use. The dialog enables
the original checkbox and persists its selection. Native write failures fail
the job instead of silently reporting partial customization as ready; see the
[behavior and failure contract](linux-windows-installer.md#hardware-requirements-option-in-the-helper-cp11q).

**CP11q completed locally on 2026-09-18:** after the host restart, fresh normal
and sanitizer builds pass. All 10 focused suites pass in both builds: 218 Qt
checks, including 26 Setup checks, plus the disposable-file helper tests.
This covers component behavior and GUI/helper integration; Windows execution of
the customization remains deferred. Temporary fixtures are removed. The earlier
host crash and recovery are recorded in the testing log.

CP11r adds the CA 2023 bootloader/file-copy path and enables its original
checkbox for eligible images. It reuses Rufus's architecture order, source paths,
answer writer and image eligibility, with native wimlib extraction and atomic
Qt file replacement inside the same authorized helper. CA-only selection does
not rewrite `boot.wim`; combined hardware selection retains both registry and
answer-file fallback paths. Copy failures fail creation instead of silently
discarding the option. See the [behavior and failure contract](linux-windows-installer.md#ca-2023-bootloader-option-in-the-helper-cp11r).

**CP11r completed locally on 2026-09-18:** all 10 affected suites pass in normal
and ASan/UBSan/leak-detection builds, with 248 Qt checks (27 new Setup checks)
plus the disposable-file helper tests. All generated fixtures are removed.
No further Windows customization installation or VM qualification was run.

CP11s enables SkuSiPolicy through the original shared answer-file command, which
runs on the installed Windows system at first logon. No native policy installer,
extra WIM rewrite or new authorization is needed. A shared mask correction fixes
an existing omission of first-logon commands when SkuSiPolicy or QoL was selected
without unrelated options. The commands themselves remain unchanged; see the
[behavior and shared fix](linux-windows-installer.md#skusipolicy-option-and-independent-first-logon-choices-cp11s).

**CP11s completed locally on 2026-09-18:** all 12 affected suites pass normally
and with ASan/UBSan/leak detection, including 255 Qt checks and the shared
answer-file regressions. Fixtures are removed. Windows runtime validation and
additional customization installation tests remain deferred.

**CP11t completed locally on 2026-09-18:** a small native reader now returns
edition indexes/labels and the Setup language from the selected ISO/UDF view,
using upstream wimlib's public metadata API and our existing reader extension.
It preserves Rufus's label and language fallbacks; full language tags are kept
instead of truncating them to seven bytes. All six focused suites pass normally
and with ASan/UBSan/leak detection. CP11w connects this input component to the
existing preflight worker; silent installation stayed disabled until CP11aa. See the
[input contract and remaining integration](linux-windows-installer.md#silent-installation-inputs-cp11t).

**CP11u completed locally on 2026-09-18:** the first physical-USB preview
exposed missing intermediate copy reports. Share the original Win32 allocation
and float arithmetic in `src/progress_policy.c`, and forward ISO/UDF block
counts through the existing reporting job and authorized helper. Copy text uses
one decimal place; the whole-job bar uses Rufus's separate weighted ranges.
Native formatter/customization stages currently advance at phase boundaries;
do not invent elapsed-time percentages for APIs without progress callbacks.
Keep the GUI unprivileged and the separate helper, with one authorization for
the complete job (privilege discussion reaffirmed on 2026-09-18). Install the
matching GUI/helper pair before the next percentage-display USB qualification;
do not replace the helper during an active creation. See the testing record
for exact arithmetic parity and seven passing suites, normally and with
ASan/UBSan/leak detection. Physical-media
completion and bootability are not established by the observed copy activity.

**Completion contract (CP11v, 2026-09-18):** assume the user removes the USB
as soon as completed READY appears. Match Win32's full green bar and single
completion alert, with “Finalizing, please wait...” while cleanup is running.
Copy completion and a received success result are insufficient: require sync,
private unmount, filesystem-provider cleanup and worker/helper exit first.
Sync/cleanup failures remain FAILED, even though Win32's `RemountVolume()` does
not propagate every `FlushDrive()` failure. This stronger error handling is
required by the READY contract. Windows Rufus flushes/refreshes the volume and
closes its handles; it does not eject or power off the USB. Linux closes its
private write mount and leaves the device connected, without a desktop remount
that could introduce new writes. Preserve completed success when the user
unplugs the device, and do not repeat the completion alert on discovery updates.

**CP11w completed locally on 2026-09-18:** eligible Windows preflight results
now carry owned edition/Setup-language metadata and the first install archive's
path through the existing worker to the GUI. Reuse the same retained ISO/UDF
view; do not choose an edition or silently substitute a later archive. An
optional metadata error leaves ordinary creation and existing customizations
available, while cancellation or source changes still abort preflight. No new
worker, callback mechanism or helper request fields are introduced. Selection
UI and helper validation were still pending at that checkpoint; CP11aa below
completes the flow for supported single-archive images.

**CP11x completed locally on 2026-09-21:** the Windows User Experience dialog
now receives the preflight edition list and presents a disabled dropdown beside
the original silent-install checkbox. It preserves native WIM indexes despite
duplicate labels and does not automatically choose an edition. A tested regional
editor collects explicit display/fallback languages, system/user locales,
keyboard inputs and a Windows time-zone ID. Qt supplies locale suggestions and
time-zone mappings; no host defaults or private mapping table are used. The
editor is not yet opened by the creation flow, and silent installation remains
disabled. Seven focused suites pass normally and with ASan/UBSan/leak detection,
including large-font and 2x-scale checks. See the
[input UI contract](linux-windows-installer.md#edition-display-and-explicit-regional-editor-cp11x).

**CP11y completed locally on 2026-09-21:** the native backend can apply a complete
silent-install answer using the existing `RUFUS_UNATTEND_OPTIONS` and shared
writer. Native registry success removes only bypass commands when silent WinPE
work remains, and commits the edited hive and answer together. Registry failure
retains the answer-file fallback. The existing ordinary hardware/OEM behavior
is preserved. Input checks reject incomplete regional data or an unusable account
before file changes. Rufus's reserved-name predicate is shared without changing
its existing writer behavior. Ten affected suites pass normally and with
ASan/UBSan/leak detection; generated WIM readback matches the original Win32
answer post-processing. At that checkpoint, the GUI/helper request path still
rejected silent installation: backend capability alone could not enable the checkbox.

**CP11z completed locally on 2026-09-21:** the existing helper request now carries
an image-internal archive path, native edition index, explicit regional fields
and confirmation of all three warnings. The helper validates inputs before
opening the target, then re-reads source metadata before partition writes. It
checks image eligibility, exact archive identity and edition existence, and uses
its own Setup-language result. A small adapter owns UTF-8 strings through the
existing shared writer/backend call. Ordinary requests retain their wire format;
old helpers reject the added silent fields, and the 4 KiB frame limit is unchanged.

The original generator's index-only selection has no explicit archive path.
Until that is resolved, silent requests require one conventional root
`sources/install.wim` or `install.esd`. Multiple or nonstandard archive layouts
remain usable for ordinary creation. This restriction prevents applying a selected
index to the wrong archive and is an interim parity gap, not a permanent omission.
Focused ISO/UDF, request and WIM-readback tests cover the new boundary. All thirteen
selected suites pass normally and with ASan/UBSan/leak detection, including the
Widgets/controller regressions. No Win32 source or build files change in this checkpoint.

**CP11aa completed locally on 2026-09-21:** silent installation is now connected
through the Qt Widgets creation flow for supported single-archive images. The
explicit regional editor sits beside the disabled Windows-host copy option.
Account/privacy choices plus valid regional inputs reveal the original silent
row; the edition dropdown has no automatic selection. Rufus's original warning
and three initially unchecked acknowledgements appear on acceptance. All three
are required, and cancellation returns to the options dialog. The normal USB
warning and one authorization still follow.

Changing choices invalidates an open warning. Device removal closes either child
dialog with its parent. The controller copies only Windows choices into the
retained request, validates them and the cached selected metadata, then performs
fresh source preflight before authorization. Silent mode, edition, regions and
acknowledgement are not persisted or reused for a new job. Shared source/selection
checks serve the GUI, controller and helper; no Windows code or answer template
changes were needed. Thirteen focused suites pass normally and with sanitizers,
including simulated GUI-to-helper transport and large-font/2x-scale screenshots.

**CP11ab completed locally on 2026-09-21:** S Mode uses the unchanged shared
answer-file writer, through the existing Linux customization backend. Qt adds
the original Ctrl+Alt+E expert-mode shortcut, 3.5-second status message and the
original checkbox/tooltip after the ordinary choices. Expert mode persists;
the S Mode selection is remembered only within the session, as specified by
Rufus's mask. Turning expert mode off hides the choice without discarding that
session preference. The controller rejects a selection that was not offered.
The helper checks actual image eligibility independently of the GUI preference.
The original incompatibility warning for online-account bypass is retained;
Win32 permits both checkboxes and Linux preserves that behavior.
Ten focused suites pass normally and with ASan/UBSan/leak detection, covering
keyboard activation, selection persistence, one-helper transport and generated
OEM/Setup-WIM answer readback. No Windows installation trial was added.

**CP11ac completed locally on 2026-09-21, archive investigation and native split qualification:** the Windows
archive picker feeds `GetEditions`, but the shared answer writes only
`/IMAGE/INDEX`; it does not identify that archive to Setup. Microsoft's explicit
path setting exists, but a path inside an ISO does not establish a media drive
or working directory in WinPE. Do not add a picker that promises a binding we
have not established. Keep the single-archive restriction, and defer expansion
until a specific Setup launch/path contract is designed and qualified. See the
[archive decision](linux-windows-installer.md#archive-binding-investigation-cp11ac).

Native `wimlib_split` is exercised directly through the unchanged upstream reader
extension. Generated ISO9660/UDF WIMs retain both editions and all file contents
after split/readback, including compressed and pipable inputs. Rufus's 4094 MiB
target and FSYNC flag remain the intended production settings. A part-size
request is not a hard bound; oversized resources and solid archives need explicit
handling. No production wrapper or GUI option was added for this qualification.
All six selected splitter/reader suites pass normally and with sanitizers.

**CP11ad completed locally on 2026-09-21, checked extraction splitting:** the native
extractor now has an explicit component option to replace conventional
`sources/install.wim` with SWM parts. Upstream wimlib still owns the split algorithm;
the Linux adapter stages on the destination filesystem, checks actual FAT32
limits, publishes without replacement, and removes its files on failure. Read-only
preflight rejects solid/spanned input, individually oversized resources, missing
or duplicate conventional archives, and source names that would collide with
numbered SWM outputs. Source identity, cancellation and Rufus's compressed-resource
progress arithmetic are retained. All eight selected suites pass normally and
with ASan/UBSan/leak detection. See [CP11ad](testing-and-checkpoints.md#cp11ad-checked-native-wim-splitting-in-iso-extraction-2026-09-21).

**CP11ae completed locally on 2026-09-21, helper selection and split-space checks:**
fresh preflight now derives the split decision from the shared Rufus filesystem
policy and a sole conventional Windows install.wim. The request carries only
`enhancedInstall`, corresponding to Windows **Alt+E** (`allow_dual_uefi_bios`),
not Ctrl+Alt+E expert mode or an instruction to split an arbitrary archive. The
helper revalidates it before partition writes and uses the resulting extraction
option through the existing single authorized job. Default mode, NTFS and small
WIM copying keep their existing behavior.

Upstream wimlib's first BEGIN_PART supplies a read-only split plan: abort before
output opening, then reserve the source size plus per-part overhead (at least
the original 2 MiB headroom). This is a conservative budget, not a duplicate
split algorithm or an exact FAT/customization allocation model. Partition-size
checks include it; extraction rechecks actual free space on the mounted target,
and actual split output must remain within the budget and FAT32 per-file limit.
No new upstream patch or full-WIM staging copy is added.

**CP11af completed locally on 2026-09-21, real FAT32 split qualification:** the
production helper created a disposable GPT/FAT32 USB from the supplied Windows
11 ISO with one polkit authorization and zero customization flags. Its
7,578,075,168-byte install.wim became two FAT32-sized SWM files. Independent
system wimlib verified all 11 images and 95,219 metadata/resource entries; image
XML, metadata and resource hashes/sizes match the original archive. All 975
ordinary files retain their source hashes. Read-only filesystem/disk checks,
sync, private unmount and helper exit pass. The source ISO was shared read-only,
preserved and rehashed; disposable disks were removed after exporting evidence.
See [CP11af](testing-and-checkpoints.md#cp11af-real-windows-wim-split-on-disposable-fat32-2026-09-21).
This qualifies media creation and data integrity, not Windows Setup consuming
the SWM files. No additional customization or installation trial was added.

**CP11ag completed locally on 2026-09-21, Qt enhanced-installation preference:** Alt+E now toggles the persistent
`EnableWindowsDualUefiBiosMode` preference and refreshes filesystem/cluster choices.
The current filesystem is preserved when still allowed, matching the Win32
repopulation behavior. Large-WIM Windows images continue to default to NTFS;
FAT32 becomes selectable when enhanced installation is enabled. Turning it off
falls back to NTFS if FAT32 is no longer allowed. Fresh preflight and the existing
helper derive whether splitting is required; the GUI sends only the preference.
Alt+E remains separate from Ctrl+Alt+E expert/S Mode and is ignored during image
analysis or an active creation flow. Status lasts 3.5 seconds, as on Windows.

Linux retains GPT/UEFI and says “Enhanced installation mode enabled (UEFI only)”
instead of Windows's “Dual UEFI/BIOS mode.” This is necessary because the Linux
BIOS/MBR installation path is not implemented; the toggle must not advertise it.
Nested/nonstandard archive layouts remain excluded from the first split route.
All seven selected GUI/policy/request suites pass normally and with
ASan/UBSan/leak detection; see [CP11ag](testing-and-checkpoints.md#cp11ag-qt-enhanced-installation-preference-2026-09-21).

**CP11ah completed locally on 2026-09-21, FAT32/SWM Setup boot:** a newly created,
independently verified USB boots directly through UEFI into Windows 11 Setup.
WinPE reports UEFI firmware type 2, sees both expected SWM files, and DISM reads
their edition metadata successfully. Setup displays all 11 editions. This uses
zero customization flags, no installation disk and no network. The new
`windows_setup.py --boot-only` mode provides that bounded test configuration;
`wim_split.py --keep-usb-for-boot` retains verified media only for this next step.
The disks and VM state were removed after saving screenshots, logs and hashes.

The boot runner ended with status 143 before its normal final checksum record;
the cause is unknown. QEMU/TPM were confirmed stopped, and a separate complete
SHA-256 check confirmed the backing USB was unchanged. Small child-process and
diskless QEMU/TPM shutdown checks pass. This qualification proves boot and edition
discovery, not Windows applying the complete split archive. Secure Boot
enforcement, BIOS and physical-media qualification remain deferred. See
[CP11ah](testing-and-checkpoints.md#cp11ah-fat32swm-boot-into-windows-setup-2026-09-21).

The Qt enhanced-installation flow and its FAT32 boot checkpoint are complete.
Do not schedule another large Windows fixture merely to repeat these checks.
Windows installation from SWM files remains deferred.

**CP11ai completed locally on 2026-09-21, VM supervisor interruption handling:**
the boot runner now saves child-process status before and after cleanup. SIGTERM
unwinds through cleanup while retaining its failure exit status; repeated
interrupts are deferred until both children have stopped. Ten small process
scenarios and normal/interrupted diskless QEMU/TPM checks pass. No Windows fixture
was recreated and no production/helper or Win32 code changed. The cause of
CP11ah's original status 143 remains unknown; the new records distinguish a VM
exit from supervisor interruption and incomplete cleanup in subsequent runs.
See [CP11ai](testing-and-checkpoints.md#cp11ai-vm-supervisor-interruption-and-exit-records-2026-09-21).

**CP12a completed locally on 2026-09-21, native Syslinux qualification:** a
standalone CMake build compiles the unchanged upstream 6.04-pre1 mtools installer.
Its loader and boot-sector bytes exactly match Rufus's bundled v6 resources;
the companion module is pinned to that same release. Root/subdirectory FAT32
installation, bounded offset writes, two BIOS volume boots and unchanged-media
rejection of 4096-byte sectors pass. All fixtures are small and removed afterward.
The shared selection tests still preserve unknown detection and inferred numeric
fallback separately. No GUI/helper/Win32 behavior changes in this checkpoint.
See [CP12a](testing-and-checkpoints.md#cp12a-native-syslinux-installer-and-fat-qualification-2026-09-21)
and the [reproducible probe](../linux/probes/syslinux/README.md).

**CP12b completed locally on 2026-09-21, native pair and root-config checks:**
the Linux component checks the qualified 6.04-pre1 pair against shared selection,
FAT32/512-byte geometry and exact companion module bytes. The inferred v6 fallback
is accepted only with the matching companion; detected facts remain untouched.
23 component cases pass normally and with ASan/UBSan/leak detection. Four generated
ISO layouts pass shared scanning/extraction, native root installation, complete
file readback and BIOS boot through real menu modules to a relative boot fixture.
The existing root config is preserved or backed up/replaced according to shared
Rufus rules. All media are removed after testing.

The native root route adds a verified root `ldlinux.c32` for bootstrap; image
modules remain at their original paths. Check any existing root companion before
overwriting it. This is an explicit native difference, qualified for the tested
layouts, not a claim about all search-path/config precedence cases. See
[CP12b](testing-and-checkpoints.md#cp12b-native-syslinux-pair-selection-and-extracted-iso-boots-2026-09-21).

**CP12c completed locally on 2026-09-21, native installer process handling:**
the existing command runner now has an explicit installer entry point for a
dedicated worker that can adopt and reap orphaned children. It passes a retained
target descriptor, a controlled environment and private working directory;
cancellation and timeout stop the entire installer/mtools process group before
final sync. A parent reporting success while leaving children behind is a failure.
Existing formatter callers retain their previous single-process behavior.
21 process cases, three native file-image cases, the four extracted-ISO boots,
earlier native FAT cases and FAT/NTFS formatter regressions pass. The new worker
also passes ASan/UBSan/leak checks. Disposable files were removed; the installer
call site remains in the standalone probe, with no helper or Qt BIOS enablement.
See [CP12c](testing-and-checkpoints.md#cp12c-native-installer-process-handling-2026-09-21).

**CP12d completed locally on 2026-09-21, read-only Syslinux source eligibility:**
the Linux adapter walks the retained analysis view with Rufus's existing ISO/UDF
walker and reader. It checks the selected/root companions, reserved boot paths,
case collisions and backup conflicts, then compares initial config lookup with
the existing Win32/native loader behavior. It uses the shared root-redirect and
EFI-copy rules; harmless extra configs remain allowed. Different initial configs
are rejected, including a Win32 root-config fallback edge case found in source.
This is a source comparison, not Windows runtime validation. Shared/Win32 code
and detected/inferred version facts remain unchanged.
46 source/fault/view checks and eight extracted-ISO boots pass. This checks the
initial route, not every command/module a config might reference. Source identity,
ordinary extraction checks and target validation remain required. See
[CP12d](testing-and-checkpoints.md#cp12d-native-syslinux-source-layout-eligibility-2026-09-21).

**CP12e completed locally on 2026-09-21, native Syslinux file adapter:**
source eligibility, retained source/target validation, private installer execution,
sync and cleanup now run as one file-only operation. Verification reuses Rufus's
FAT reader and Syslinux patcher to reconstruct and compare the entire installed
loader/ADV and primary boot sector, preserving the original BPB and backup VBR.
The root companion must match the qualified payload. The only shared patcher
change replaces six unaligned integer accesses with memcpy after UBSan exposed
them; optimized x86-64 output remains single loads/stores. Windows runtime
validation remains deferred. 32 adapter cases, 24 packed-field cases and eight
extracted-ISO boots pass, including sanitizer/leak checks on the new adapter.
See [CP12e](testing-and-checkpoints.md#cp12e-native-syslinux-file-installer-adapter-2026-09-21).

**CP12f completed locally on 2026-09-23, Syslinux MBR whole-disk images:**
the existing partition component now creates/checks one active FAT32 LBA partition
using libfdisk, Rufus's shared placement calculations and the exact Syslinux MBR
bytes used by Win32. It preserves descriptor ownership/position, flushes after
attempted writes and independently reads back the table and boot code. 57 planning,
fault and readback calls pass normally and with sanitizers/leak checks. Two new
whole-disk fixtures boot from MBR through FAT32/Syslinux/menu; the eight prior
volume boots and GPT/FAT formatter regressions remain green. This is ordinary
512-byte-sector MBR layout, without old-BIOS fixes, extra partitions or device
support. Existing non-DOS labels are rejected; clearing old metadata is a separate
full-job responsibility. No Win32/shared code changed in this checkpoint. See
[CP12f](testing-and-checkpoints.md#cp12f-syslinux-mbr-whole-disk-images-2026-09-23).

**CP12g completed locally on 2026-09-23, native Syslinux packaging:** the main
Linux build and standalone qualification now use one CMake recipe for the
unchanged upstream installer and pinned payloads. `RUFUS_SYSLINUX_UPSTREAM`
selects this optional dependency; `SyslinuxRuntime` installs the private
`rufus-syslinux`, upstream license and provenance notice. A staged package passes
two whole-disk boots and three altered-payload configuration rejections; all
seven standalone suites remain green. No host installation, new privilege
action, helper invocation or BIOS enablement was added. CI does not yet select
this optional dependency. See
[CP12g](testing-and-checkpoints.md#cp12g-native-syslinux-cmake-packaging-2026-09-23).

**CP12h completed locally on 2026-09-23, MBR device preparation:** the partition
backend now accepts a retained, exclusively owned block descriptor for Syslinux
MBR creation/readback. An explicit preparation entry point applies Rufus's disk
and new-partition-prefix clearing ranges, verifies them after sync, then creates
the MBR. The clearing-size constant is shared with Windows without changing its
value or behavior. Linux fails on tail clearing errors instead of leaving a
possible backup GPT behind. 26 preparation/readback and 57 existing MBR calls pass
with sanitizers/leak checks; eight guest block requests qualify GPT conversion,
kernel partition refresh, retained ownership and unchanged rejected 4K media.
See [CP12h](testing-and-checkpoints.md#cp12h-syslinux-mbr-device-preparation-2026-09-23).

**CP12i completed locally on 2026-09-23, MBR formatting/mount handoffs:**
`PrepareDeviceTarget` now takes an explicit table type, retaining GPT as the
default. Syslinux MBR jobs preserve/check sector zero without treating valid
filesystem data at the disk's end as a backup GPT. Existing CLI formatting,
private mounting, sync/unmount and exclusive-claim reacquisition are reused.
Failed snapshots cannot authorize mounting, and metadata is checked before
mounting as well as after reacquisition. Thirteen guest cases pass normally and
with sanitizers/leak checks, including a full FAT32 volume independently read
back through the disk's final sector. No helper protocol or GUI route is enabled.
See [CP12i](testing-and-checkpoints.md#cp12i-mbr-formatting-and-mount-handoffs-2026-09-23).

**CP12j completed locally on 2026-09-23, native Syslinux block installation:**
the existing installer adapter now accepts the prepared MBR layout and disk
sequence with a retained whole-device descriptor. It reuses the FAT validator,
native process runner and Rufus loader/VBR verification. The exclusive disk
claim stays held throughout the unchanged upstream installer and mtools children;
no formatting-style claim handoff or additional authorization is needed.
Ten normal/sanitized guest cases pass, including device/layout refusals,
cancellation, sync failure and changed MBR. Independent file readback and an
MBR/Syslinux/menu boot pass. This is a generated boot fixture, not a distribution
qualification or application START. See
[CP12j](testing-and-checkpoints.md#cp12j-native-syslinux-block-installation-2026-09-23).

**CP12k completed locally on 2026-09-23, complete Syslinux helper job:** an
explicit MBR/FAT32 request now uses the existing authorized helper from fresh
source/policy/tool preflight through partitioning, CLI formatting, shared ISO
extraction, sync/unmount, native installation, final verification and helper exit.
The request cannot provide executable paths or combine Syslinux with Windows/NTFS
options. The same phase/progress protocol and cancellation channel are reused;
installation runs during finalization before success. Five normal/sanitized
production-helper jobs cover missing tools, incompatible source modules,
cancellation during installation, and known/unknown source-version success.
Independent readback and whole-disk BIOS boots pass. See
[CP12k](testing-and-checkpoints.md#cp12k-complete-syslinux-helper-job-2026-09-23).

**CP12l completed locally on 2026-09-23, real distribution qualification and
source-version compatibility:**
Alpine 3.22.5 exposes an overly strict initial check: its rebuilt 6.04-pre1
companion differs from the upstream binary, while Windows Rufus selects the
loader by version and suffix. The Linux source checker now follows that rule for
known versions and preserves the non-root distribution module. Unknown-version
inference, root replacement and installed-payload verification retain exact
identity checks. The unchanged ISO passes complete helper creation on a 512-MiB
simulated USB, independent readback and BIOS boot into Alpine userspace. See
[CP12l](testing-and-checkpoints.md#cp12l-real-alpine-bios-qualification-2026-09-23)
for validation and its limits.

**CP12m completed locally on 2026-09-23, Qt BIOS selection and creation:** the partition combo now offers the
qualified MBR/FAT32/Syslinux route alongside GPT/UEFI for suitable images. Hybrid
images default to MBR, retain the original ISO/DD choice and select ISO by default;
an explicit partition choice is preserved. Source/config/payload compatibility
and fixed installed tools are checked before confirmation/authorization, with a
second tool check before launch and the helper's independent preflight. Unknown
detection remains unknown until exact native-pair eligibility succeeds. Creation
uses the existing one-helper lifecycle, Win32 progress allocation and final
sync/cleanup/exit gate. Linux now uses the effective unpadded FAT label after
shared Rufus normalization, so truncating Alpine's default label cannot veto it
or introduce padding into boot configuration patches. Windows code is unchanged.

CP12m initially limited the MBR target wording to **BIOS (or UEFI-CSM)** until
the additional firmware path was qualified. CP12p below closes that gap:
compatible images with regular EFI files show **BIOS or UEFI**, matching Win32;
BIOS-only images retain the original BIOS/CSM label. GPT/UEFI remains selectable.
The idle lower bar follows Win32's **Using image: filename.iso**. Diagnostic
details remain in the log/tooltip; usable images show empty READY, while completed
creation remains full READY only after helper exit.

**CP12n GUI behavior audit (2026-09-23):** compared image-load and selection
transitions with Win32's `ImageScanThread`, `UpdateImage`, `EnableControls`,
`EnableBootOptions`, `SetFileSystemAndClusterSize` and creation/completion
handlers. Qt now restores START focus, shows the Windows installation option
row, clears rejected scans, rejects images with no recognized boot method,
explains and disables formatting for DD-only images, retains a manual filesystem
choice across device changes, returns to READY when a pre-write confirmation is
declined, and displays the elapsed creation time. Marquee captions stay visible.
The [GUI audit](linux-gui-parity.md) records tested behavior and remaining feature
gaps; it does not claim full Windows feature or pixel parity.

**CP12o completed locally on 2026-09-23, installed runtime qualification:** CMake
now installs the GUI as well as the helper and optional native Syslinux runtime.
The unchanged generated polkit policy and `/usr` installation pass in a disposable
guest. The installed executable starts/closes as UID 1000 without authorization;
the actual Widgets/controller harness then uses live UDisks and the installed
production helper for cancelled and successful Alpine creation. Each job requests
authorization exactly once. Cleanup, full READY after helper exit, independent
file/boot-payload readback and a whole-USB BIOS boot into Alpine userspace pass.
See [CP12o](testing-and-checkpoints.md#cp12o-installed-runtime-and-widgets-2026-09-23)
for the distinction between executable startup and the instrumented Widgets jobs.

**CP12p completed locally on 2026-09-23, MBR/UEFI and cancellation wording:** the
same MBR/FAT32/Syslinux USB passes independent readback and both SeaBIOS and OVMF
boots into Alpine userspace. No new boot implementation is needed for its copied
EFI files. The Qt target and confirmation now say **BIOS or UEFI** when regular
EFI files are present; BIOS-only images still say **BIOS (or UEFI-CSM)**. DD and
ISO cancellation reuse Win32's short **Operation cancelled by the user** text
in the lower bar, with full diagnostics retained in the tooltip/log. Secure Boot,
other distributions/architectures and physical firmware are not inferred from
this result. The Alpine embedded-label warning remains visible and documented.

**CP13 completed locally on 2026-09-23, desktop integration:** CMake stages the
menu entry and eight unchanged upstream icons alongside the GUI/helper. Qt uses
the matching desktop identity. A fresh Release build with tests disabled passes;
the staged launcher starts and closes as UID 1000 on an isolated X11 display,
with matching window identity and icon. A separate GUI-off build passes and
installs no desktop files. The [packaging guide](../linux/packaging/README.md)
records configured paths, runtime dependencies and installation limits. CI now
includes staged desktop validation; remote CI and native Wayland were not run.
The temporary packaging builds/stage and completed Alpine input are removed;
their source/hash, manifests and validation evidence remain.

**CP13a completed locally on 2026-09-23, minimum Qt qualification:** the full
application, helper and tests build against the official Qt 6.2.4 SDK. All 74
hardware-free CTest entries are verified: 73 passed initially; the remaining
preflight test passes after initializing Qt's regular-expression engine before
its descriptor-leak baseline. The unchanged per-preflight leak checks remain.
Its 106 cases also pass with the host's Qt 6.11.2, as do all 11 current-version
Widgets test entries. Staged Qt 6.2 X11 desktop launch/close passes as UID 1000.
No application compatibility shim was required. CMake now reports its Qt version;
the build guide explains isolated SDK selection. This qualifies Qt 6.2.4 on the
current x86-64 host, not every 6.2 patch or the Ubuntu CI environments.

**CP13b completed on 2026-09-23, support documentation:** the
[current support table](linux-support.md) maps GUI routes to source eligibility,
runtime requirements, tested examples and remaining limits. The README no longer
describes Syslinux BIOS creation, Alt+E integration or FAT32/SWM Setup discovery
as future work, and older component notes point to the current workflow.
The audit checks source and existing test/boot evidence; it adds no media feature
or new boot/customization qualification. Notably, ISO/hybrid inputs currently
require the installed helper to enable START even when DD will be selected.

**CP14 completed on 2026-09-23, initial device selection:** `GetDevices` sorts
Windows targets by capacity and chooses the first when no prior drive matches.
Qt now orders eligible devices by capacity and selects the smallest on the first
populated list, including arrival after an empty startup or initial discovery
failure. Healthy selections survive smaller-device arrival and cosmetic refresh.
Linux retains explicit reselection after identity loss: a replacement or UDisks
restart must not silently rebind the old target, reset active cancellation or
erase completed READY. Eligibility and pre-write identity checks are unchanged;
selecting a device does not authorize or write. See the
[GUI audit](linux-gui-parity.md) for this deliberate refresh difference and the
[test record](testing-and-checkpoints.md#cp14-initial-device-selection-2026-09-23).

**CP14a completed on 2026-09-23, filesystem display:** Qt marks
the target's first supported native format with “(Default)” before image
filtering. This marker is independent of the current choice or ISO preference;
Windows images requiring NTFS can therefore show plain “NTFS”. Linux defaults
only to implemented FAT32/NTFS formats, rather than Windows's FAT16 default on
small media. The [GUI audit](linux-gui-parity.md) records that capability
difference. The shared image/filesystem policy and cluster calculations remain
unchanged. Generated-image Widgets checks cover capacity boundaries, changing
targets and choices, image filtering and normal/enlarged display.
CP14a initially also copied Windows's “Large FAT32” label; CP14b removes that
distinction following the user's Linux-specific naming decision below.

**CP14b completed on 2026-09-23, idle format defaults:** Win32 populates the
filesystem/cluster lists from the device before an image is selected, but leaves
them disabled. Qt now does the same for its native routes. Rejected scans restore
those lists and an allowed manual filesystem choice instead of leaving them
empty. No-image START, DD restrictions, scan/write control locking and the
authorization gates are unchanged. The FAT32 label is now plain at every size;
the Windows-only large-format constant returns to its original header.

**CP14c completed on 2026-09-23, idle partition/target preview:** Win32 uses the selected device's
existing table before image selection, with MBR for raw/unknown layouts. Qt now
reads UDisks `PartitionTable.Type` as display-only metadata and shows the same
disabled MBR/GPT and firmware defaults. Content refreshes preserve device
identity; image loading still chooses only supported native creation routes.
This preview neither selects Syslinux nor enables blank-device formatting.

**CP14d completed on 2026-09-24, volume-label presentation:** Qt uses existing UDisks
filesystem labels, with Win32's decimal capacity proposal for unlabeled media.
The first filesystem by partition offset supplies the label (or the whole-disk
filesystem for unpartitioned media). This provides deterministic ordering where
Windows drive letters do not exist; an unlabeled first filesystem does not borrow
a later label. Manual edits survive refresh/device changes and rejected scans;
an accepted new ISO resets the edit and supplies its own label. Raw DD labels
stay empty. These display facts never become physical identity or authorization.
No mount or device read is requested for discovery. The `autorun.inf` extended
label path remains separate from filesystem-label support.

**CP14e completed on 2026-09-24, existing-filesystem preselection:** discovery supplies
filesystem type/version from the same volume as the label. Before image loading,
Qt preselects supported NTFS or known FAT32, unless a manual choice takes
precedence. Unknown/unsupported formats fall back to the existing native policy;
an unknown FAT version is not classified as FAT32. Content refreshes update idle
automatic choices without changing identity; loaded-image settings and active
jobs are preserved. Image policy, capacity checks and default markers remain
independent of the detected format.

**CP15a completed on 2026-09-24, FAT16 regular-file formatting:** the native formatter now
accepts FAT16 through the same descriptor checks, mkfs.fat process supervision,
cancellation, sync and libfat readback used by FAT32. Shared request fields retain
the existing FAT32 API. FAT16 adds conservative cluster-count bounds, fixed-root
directory geometry and its boot-sector fields; no second process runner or FAT
reader is introduced. Independent fsck.fat/mtools checks cover small volumes,
64 KiB clusters, offsets, odd sizes, labels, failure/cancellation and corrupt
readback. This qualifies regular files only; FAT16 is still unavailable in GUI
and device creation. Conservative preflight may reject marginal geometries
before mkfs; it does not claim every theoretically valid FAT16 volume.

**CP15b completed on 2026-09-24, FAT16 policy and GPT regular-file integration:**
Rufus's FAT16 capacity/cluster algorithm is shared with the original Windows
caller. Linux filters those choices against the actual partition extent and
native formatter limits. GPT remains filesystem-independent: the same Basic Data
type, placement and readback apply, with clusters up to 64 KiB even without a
UEFI:NTFS payload. FAT32 keeps its formatter limit. Twenty-two sparse-disk cases
cover every offered cluster at representative capacities from 16 MiB through
the last sector below 4 GiB, with independent GPT/FAT checks, sfdisk, fsck.fat
and mtools. This verifies formatted storage, not bootability. FAT16 remains
unavailable to GUI/device jobs; Syslinux MBR remains the qualified FAT32 route.

**CP15c completed on 2026-09-24, FAT16 regular-file preparation job:**
`PrepareImageRequest::fileSystem` now uses explicit `FS_*` values, defaulting to
FAT32. Existing FAT32/NTFS/Syslinux operation names are preserved; the new
`prepare-fat16-image-v1` operation is rejected by old helpers. FAT16 uses the same
GPT preparation worker, formatter runner, locking/identity checks, cancellation,
sync, result validation and exit-gated completion. Tests cover independent
filesystem readback, FAT16/FAT32 process reuse, rejected requests, formatter
failure/false success, EOF cancellation, child reaping and flush failures before
and after formatting. Device and ISO-copy entry points explicitly reject FAT16
until their integration is qualified. No new job/backend abstraction was added.

**CP15d completed on 2026-09-24, authorized FAT16 device preparation:**
the existing GPT device job now accepts explicit FAT16 format-only requests.
It reuses FAT32's retained partition descriptor, mkfs.fat exclusive-open handoff,
sync, disk reacquisition and partition-table revalidation. A 64 MiB virtual USB
passes all nine existing authorization/target/cancellation scenarios, each with
one request. Independent host checks confirm GPT/FAT16 metadata, 2 KiB clusters,
label/ID, hidden sectors, fsck.fat/mtools and preserved surrounding bytes.
Normal and sanitizer regressions pass. ISO copying, MBR/Syslinux and GUI exposure
remain gated; no bootability is inferred from this preparation result.

**CP15e completed on 2026-09-24, FAT16 GPT/UEFI ISO extraction:** explicit FAT16
selection now passes through ISO preflight and the existing authorized device
request. It reuses the vfat mount, copy, config patching and finalization code.
One generated-image guest qualifies source rejection, cancellation, caller death,
mount/flush/unmount failures and successful creation, with one authorization per
job. Independent GPT/FAT16 and file-hash readback passes. Fresh OVMF boots the
created USB to Linux userspace and checks every payload hash and patched config;
a missing-payload control fails as expected. Ten normal and eight sanitizer
suites pass. No Windows source changed, and no additional Windows customization
qualification was performed. This establishes the generated x64 UEFI route,
not general firmware/distribution compatibility or GUI START.

**CP15f completed on 2026-09-24, FAT16 Qt integration:** the GPT/UEFI GUI offers FAT16 when its native
geometry and shared image policy permit it. The ISO controller carries explicit
filesystem selection into preflight and the helper request. As in Win32, the
first supported format gets the default marker; an eligible EFI ISO still
prefers FAT32 when available. Known existing FAT16 is preselected before image
loading. Confirmation, label guidance and formatting status name FAT16. Focused
Widgets tests cover selection, capacity limits, source reloads, helper requests,
cancellation/failure and completion after helper exit. MBR/Syslinux remains gated.

**CP15g completed on 2026-09-24, FAT16 MBR layout and regular-file formatting:** the existing MBR
options now carry explicit filesystem selection. FAT16 uses Win32's `0x0e`
partition type and permits 64 KiB clusters; FAT32 retains `0x0c` and its current
cluster limit. Both reuse Rufus placement/clearing arithmetic, the original MBR
boot code, libfdisk creation/readback and the native formatter. Sparse-file
qualification covers metadata, surrounding bytes, formatting and failure paths.
This is not a FAT16 Syslinux boot qualification: installer, device/helper and
GUI MBR routes remain FAT32-only.

**CP15h completed on 2026-09-24, FAT16 file-only Syslinux installation:** the
adapter uses explicit filesystem selection, the FAT16 validator and no FAT32
backup-sector lookup on FAT16. Existing libfat code handles its fixed root and
cluster chains. The upstream installer and Rufus payload/patcher remain unchanged;
exact installed loader, ADV and VBR readback is retained. All 34 FAT16 and 34
FAT32 file-only cases pass, including offset/64 KiB-cluster FAT16 installation,
corruption, cancellation and cleanup failures. Our instrumented adapter passes
with the normal upstream executable, as in earlier qualifications. Instrumenting
the upstream executable itself exposes unaligned accesses and exit-time leaks;
those findings are retained, not suppressed or counted as a clean upstream run.
Device/helper and GUI Syslinux paths still require FAT32.

**CP15i completed on 2026-09-24, FAT16 authorized MBR/Syslinux device job:**
the selected filesystem now reaches shared preflight, partitioning, formatting,
installation and whole-disk readback. A distinct FAT16 Syslinux protocol operation
prevents older helpers from interpreting the request as FAT32. It uses the
existing single authorized helper, formatter claim handoff, private mount cleanup,
retained installer claim and sync-before-success rules. FAT16 passes 13 handoff,
11 block-installer and five complete helper cases in normal and ASan/UBSan guests.
Independent readback and generated BIOS boots pass; the FAT32 complete-helper
regression also boots. Native upstream installer code is unchanged and runs
uninstrumented at the documented CP15h sanitizer boundary. Qt selection stays
gated until the next focused UI checkpoint.

**CP15j completed on 2026-09-24, FAT16 MBR/Syslinux in Qt:** native cluster
choices use the selected FAT type and MBR extent. MBR eligibility accepts a
supported FAT16-only target as well as FAT32. Rufus's shared image policy still
prefers FAT32 when both fit; manual FAT16 choices survive partition/target changes,
while an accepted image scan reapplies the image preference. Confirmation and
formatting status name the selected FAT type. Both paths retain the same ISO/DD
choices, progress allocation, one-helper lifetime and READY-after-exit behavior.
Focused Widgets tests cover both known and unknown compatible Syslinux sources,
capacity limits, reloads, failures, cancellation and completion/unplugging.

**CP16a completed on 2026-09-24, native exFAT file formatting:** the small adapter
uses exfatprogs through the existing supervised CLI runner, with explicit cluster
size/sector size/Unicode label, native metadata readback, no discard and no native
partition-table creation. It formats exact-size, partition-sized regular files;
the native CLI has no offset/length option. libblkid supplies filesystem/label and
boot-checksum validation; our checks add requested geometry and matching boot
copies. Runtime libblkid 2.39+ is required for checksums (older versions reject
before writes). All 64 file-only cases pass normally and under ASan/UBSan, covering
512-byte through 32-MiB clusters, labels, quick-format free-data preservation,
retained descriptors, cancellation, failures and sync. FAT16/FAT32/NTFS regressions
pass. exfatprogs 1.4.3 and libblkid 2.42.4 are the locally qualified providers.

**CP16b completed on 2026-09-24, shared exFAT policy and geometry preflight:**
Win32 and Linux now call the extracted original exFAT allowed/default calculation,
including sector filtering and the strict 256-MiB/32-GiB default transitions.
Linux filters those choices against the actual volume while retaining the
whole-disk default. A small allocation-free preflight checks native FAT/cluster
limits and space for the bitmap, standard upcase table and root directory. The
CLI receives explicit 1-MiB alignment to match the checked layout. Impossible
requests fail before starting a formatter. Native boundary/readback tests,
shared-policy cases and sanitizer checks pass; real Windows validation is deferred.

**CP16c completed on 2026-09-24, native exFAT partition/claim qualification:**
the block adapter checks the expected partition device, capacity, sector size,
read-only state and sysfs start before and after formatting. Native exfatprogs
writes the offset metadata itself. The GPT preparation backend shares FAT's
existing release/reacquire path, including table snapshots, cancellation and
failure cleanup. Nineteen guest cases pass normally and under ASan/UBSan,
including kernel read-only state, native O_EXCL refusal, busy reacquisition,
changed GPT copies, sync failure and success with outside bytes preserved.
4-KiB-sector devices are rejected unchanged. FAT16/FAT32 handoff regressions pass.
Only 512-byte sectors and whole-MiB partition starts are qualified. No mount/copy
or complete helper operation is exposed for exFAT yet.

**CP16d completed on 2026-09-24, non-bootable GPT/exFAT helper job:** a distinct
`format-exfat-device-v1` request accepts only a device, label and cluster choice;
it rejects ISO sources, boot/customization options and supplied serial numbers.
Before changing the GPT, check metadata space, target identity/use and native
formatter capabilities. Missing/older tools fail unchanged. Expand the generic
GPT layout adapter through exFAT's 32-MiB cluster limit; individual formatters
retain their own limits. Reuse GPT creation, the format claim handoff and the
existing authorized helper. A final whole-disk sync and identity/use check precede
success; the client requires finalizing and process exit. Eleven complete jobs
pass normally and with sanitizers in an offline guest, including provider and
request refusals, formatter failure/cancellation, final-sync failure/cancellation,
EINTR and success. Independent readback checks GPT and native exFAT metadata.
No Windows code changes, new authorization or ISO-copy path is needed.

**CP16e completed on 2026-09-24, non-bootable GPT/exFAT in Qt:** Boot selection
now offers Non bootable before Disk or ISO image, retaining the selected image
when switching modes and disabling SELECT in non-bootable mode. The qualified
route offers GPT/exFAT with BIOS or UEFI, editable label and native-filtered
cluster choices using the original whole-disk defaults. START reuses the existing
creation controller, runs device/geometry/provider checks before confirmation
and again before authorization, and sends CP16d's source-free request. Shared
Win32 label normalization and non-bootable progress allocation are reused;
finalizing stays below full READY until sync and helper exit succeed. Focused
Widgets tests cover selection, refusal, cancellation, failures, exit gating and
unplug after success. ISO/DD choices and existing creation routes remain intact.
This is a temporary GPT/exFAT-only supported-route restriction: Windows also
offers other non-bootable layouts/filesystems and normally defaults to MBR when
there is no existing preference. Linux must qualify those before exposing them.

**CP16f backend checkpoint (2026-09-24):** source-free GPT/FAT16 and FAT32
jobs now reuse the existing native formatters and exFAT's final whole-disk sync.
Explicit format operations distinguish complete non-bootable jobs from the
existing preparation/ISO/Syslinux contracts. They reject sources, boot options,
Windows customization and regular-file targets. FAT keeps its caller-specified
volume serial; exFAT continues to generate its own. The preparation client
requires finalizing and helper exit. Native FAT track rounding is retained and
checked independently, rather than requiring exFAT's exact volume length.
The existing Qt filesystem choices remain GPT/exFAT until the next checkpoint.
See the testing record for native guest completion and readback evidence.

**CP16g completed on 2026-09-24, non-bootable GPT/FAT16 and FAT32 in Qt:**
The existing source-free creation controller now accepts CP16f's explicit FAT
format jobs. It reuses native geometry checks and verifies the configured
formatter before confirmation and again before authorization. Non bootable
lists FAT16, FAT32 and exFAT where native geometry permits, in original order,
with the first available format marked Default. Existing supported filesystems
and manual choices retain precedence. The same shared label normalization and
progress allocation apply. Confirmation names the selected filesystem and no
image; retained ISO/DD and Windows preferences cannot enter the format request.
Small FAT formats use five filesystem steps; large FAT32 keeps the original
analog weighting. Full READY requires sync, validated outcome and helper exit.
Window/default and functional creation/failure/cancellation tests cover all three
formats, including a retained Windows image and simulated 64-GiB FAT32 target.

**CP16h completed on 2026-09-24, non-bootable GPT/NTFS backend:** the explicit
`format-ntfs-device-v1` operation creates one data partition, with no UEFI:NTFS
partition/payload, image source or mount. Reuse the existing NTFS formatter and
metadata checks, keeping the exclusive whole-disk claim throughout formatting.
Bootable NTFS retains its boot partition and embedded payload. Both paths reject
incompatible boot-partition layouts before device access. Non-bootable success
requires final whole-disk sync, target recheck and joined helper exit. Native
normal/sanitizer guest tests cover refusals, failures, cancellation, interrupted
sync, independent NTFS readback and the existing bootable payload. Qt remains
FAT16/FAT32/exFAT-only in non-bootable mode until the next checkpoint.

**CP16i completed on 2026-09-24, non-bootable GPT/NTFS in Qt:** Non bootable
now offers FAT16, FAT32, NTFS and exFAT where native geometry permits, in original
order. Existing supported filesystems and manual choices retain precedence.
NTFS cluster filtering and preflight use one data partition without UEFI:NTFS;
NTFS label normalization preserves case/Unicode and the label tooltip names the
selected format and limit. Check the configured native formatter before
confirmation and again before authorization. The same source-free controller
and one-helper lifecycle handle creation. NTFS uses the original twelve
filesystem steps and two finalization slots; only image jobs get the extra NTFS
finalization slot. Require exact whole-volume formatting and helper exit before
successful READY. Bounded Qt tests cover selection/defaults, Unicode labels,
retained Windows-image preferences, provider refusal, failures, cancellation and
result-before-exit waiting. CP16h supplies the native simulated-USB evidence.

**CP16j completed on 2026-09-24, non-bootable MBR table backend:** the existing
Linux MBR planner/writer/clearing/readback functions now have general MBR names
and an explicit Syslinux choice. Existing callers retain Syslinux behavior;
plain FAT16/FAT32/NTFS/exFAT tables use an inactive first partition, types 0x0e,
0x0c and 0x07 respectively, and the original Win32 zero-MBR payload. This payload
zeros all first 446 bytes, including the disk signature. The 55-AA marker and
partition table remain. Reuse common Rufus placement/track/cluster arithmetic,
libfdisk and bounded stale-GPT/partition-prefix clearing. Table creation still
refuses an existing GPT; explicit preparation performs the required clearing.
Syslinux installation refuses a non-Syslinux layout. Regular-file tests cover
both modes, geometry through 2 TiB without allocating large images, readback,
metadata corruption, descriptor preservation, failures, interrupted I/O, sync
and cancellation. This checkpoint writes/verifies tables, not filesystems;
native formatter geometry/capacity must still be checked by creation callers.
No non-bootable MBR device job or GUI route is exposed yet.

**CP16k completed on 2026-09-24, non-bootable MBR/FAT16 and FAT32 device jobs:**
The explicit `format-fat16-mbr-device-v1` and `format-fat32-mbr-device-v1`
operations reuse the existing authorized helper, native FAT formatter/claim
handoff and CP16j clearing/table writer. The device adapter now names the table
MBR independently of Syslinux; boot code and installation still depend on the
verified job. Requests reject image sources, boot/customization options and
regular-file targets. MBR/NTFS and exFAT operations remain unavailable. Existing
GPT and Syslinux protocol operations retain their meanings. The MBR jobs preserve
caller-specified FAT volume serials, use inactive partitions/zeroed MBR code,
and finish only after final whole-disk sync, target recheck and helper exit.
Native guest tests cover both FAT types, including GPT conversion, refusal before
writes, formatter failure/cancellation, MBR tampering, competing claims, sync
failure/cancellation and interrupted sync. Independent sfdisk/FAT/fsck/mtools
readback verifies the result. Non-bootable Qt remains GPT-only for now.

**CP16l completed on 2026-09-24, non-bootable MBR/NTFS device jobs:**
The explicit `format-ntfs-mbr-device-v1` operation reuses CP16j's plain MBR
clearing/layout writer and the existing native NTFS formatter. It retains the
whole-disk exclusive claim through formatting and volume/table readback, uses
one inactive type-0x07 partition, and omits UEFI:NTFS, image and mount work.
Native mkntfs generates the volume serial. Source-free request boundaries and
final sync/target recheck/helper-exit semantics match the other complete format
jobs. Existing bootable GPT/NTFS keeps its payload; bootable MBR/NTFS remains
unsupported. Native qualification and results are in the checkpoint log.

**CP16m completed on 2026-09-24, non-bootable MBR/exFAT device jobs:**
The explicit `format-exfat-mbr-device-v1` operation reuses plain MBR clearing,
the existing exfatprogs formatter and its exclusive-claim handoff. It creates
one inactive type-0x07 partition with zeroed MBR boot code/signature. Requests
remain source-free with native serial generation. Geometry, metadata space and
tool capability checks precede writes; table/volume readback and whole-disk
sync precede successful helper exit. No image, mount, installer or additional
authorization path is added. Native qualification is recorded in the test log.

**CP16n completed on 2026-09-24, non-bootable MBR formatting in Qt:**
Non bootable now offers MBR and GPT with FAT16, FAT32, NTFS and exFAT, subject
to native geometry. It defaults to MBR independently of the existing table,
remembers explicit choices, and switches the remembered preference to GPT above
2 TiB, following Win32 SetPartitionSchemeAndTargetSystem. Both layouts display
BIOS or UEFI. Native cluster choices and preflight use the selected layout;
confirmation names it and the controller sends the corresponding qualified
source-free helper operation. Progress weighting, one authorization and
READY-after-sync/helper-exit are unchanged. The Linux combo omits MBR above
2 TiB because its 512-byte-sector backend cannot represent that extent; Windows
still lists MBR and handles the limit later. SFD is not exposed yet.

**Scope revision (2026-09-25): full Windows parity, including SFD.** This
supersedes the earlier decision to postpone SFD behind the main workflow.
CP16o implements whole-device preparation and FAT32 file qualification. CP16p
qualifies the native SFD/FAT32 device handoff and stale kernel partition removal.
CP16q qualifies the complete source-free SFD/FAT32 helper job, including one
launcher, cancellation, final sync and waiting for helper exit. CP16r exposes
the non-bootable Qt choice with Windows naming/defaults, limited to qualified
FAT32 geometry. Next is FAT16 SFD native/helper qualification (CP16s), then its
Qt availability.
FAT16/NTFS/exFAT SFD routes remain in scope with their own native checks.
Arbitrary file subranges remain unsupported, and exFAT remains excluded from
general ISO extraction according to the original Rufus boot-mode restrictions.

**CP17a completed (2026-09-25), DD independence from the preparation helper:**
An ISOHybrid/DD job uses UDisks and does not need the ISO preparation helper.
Enable START when an offered mode has its required runtime. Retain Rufus's
ISO/DD choices and recommended ISO default; a missing helper must not silently
force DD. DD-only images still skip the choice dialog. Choosing ISO without its
helper gives an actionable error before authorization or writes. Recheck helper
availability before the initial ISO/non-bootable preflight and again after
confirmation. Existing source identity checks and UDisks authorization/sync/
verification remain intact.

**Planned product checkpoint (CP17b): mounted-target handling.** Audit the existing
UDisks refusal rules against the normal desktop case where a removable USB
volume is already mounted. Add a bounded backend step for unmounting eligible
target filesystems within the single authorized job, while continuing to reject
swap, encrypted/layered or busy/system devices and source-on-target layouts.
Use simulated devices to verify refusal before writes, cancellation and no
extra authorization. Integrate it into creation only after those checks pass;
keep the GUI unprivileged. No new Windows customization qualification is needed.

**Filesystem backlog and naming decision (2026-09-23, updated CP16n):**
FAT16 and exFAT were retained as Linux parity work. FAT16 ISO routes are now
implemented; exFAT and standalone formatting remain below.

- [x] FAT16 for the supported ISO routes: CP15a–j cover regular-file formatting, shared cluster policy,
  GPT integration, authorized device preparation and generated-image UEFI copying
  and boot, GPT/UEFI GUI selection and requests, and MBR layout/formatting on
  regular files, native Syslinux installation on file volumes, and authorized
  MBR device creation with generated BIOS boot qualification and Qt selection.
  CP16f/g add non-bootable GPT/FAT16 and FAT32 helper jobs and Qt exposure.
  CP16k/n add non-bootable MBR FAT jobs and Qt exposure. SFD remains separate.
  Retain filesystem, label/cluster, failure/cancellation and sync checks.
- [ ] exFAT: CP16a–e complete native regular-file formatting, labels, shared
  cluster/default policy, native geometry preflight, readback, failure/cancellation
  and sync/cleanup, plus GPT partition formatting, claim handoff and a complete
  non-bootable helper job with Qt exposure. CP16m/n add MBR/exFAT helper and Qt
  support. SFD and dedicated boot modes remain.
  Follow Rufus's boot-mode restrictions: exFAT support does
  not make it a general ISO-extraction option. Add non-bootable formatting and
  any dedicated supported boot mode as separate, testable integration steps.

Both routes must retain target identity/use checks, one authorization per job
and the existing READY-after-sync/cleanup rule. Unimplemented routes keep their
controls disabled until the corresponding job works. Neither requires another Windows customization
qualification. The user chose the plain **FAT32** label for every supported Linux
volume size: `mkfs.fat` handles both small and large volumes, so Windows's formatter
switch and “Large FAT32” label do not apply. This supersedes CP14a's capacity-based
name only; capacity/cluster validation and the separate default marker remain.

Real desktop automount, hot-unplug and physical USB remain separate qualifications.
Existing Windows UEFI paths and the single-customization qualification remain
unchanged. See [CP12m](testing-and-checkpoints.md#cp12m-qt-syslinux-creation-2026-09-23).
Keep focused component tests; do not
schedule more customization VM/installation tests unless the user asks.
Keep the option to copy this user's Windows regional settings visible but
disabled on Linux, with a clear tooltip; do not substitute Linux locale/timezone
values (decision 2026-09-17). All other Windows ISO customization options remain
in scope for Linux parity, including boot.wim/offline-registry changes, silent
installation, local accounts, online-account and hardware-requirement options,
privacy/QoL, BitLocker, S Mode and the newer bootloader/policy choices. Implement
these in small checkpoints; use native libraries/adapters where the Windows
backend uses host APIs. Windows To Go, persistence and Windows BIOS installation
remain unimplemented. CP11ad connects native WIM splitting, CP11ae connects helper
eligibility, CP11af qualifies real FAT32 media creation, CP11ag exposes the
separate enhanced-installation preference in Qt, and CP11ah observes UEFI boot
and Windows Setup edition discovery from that media. Win32 Rufus
application runtime validation remains deferred; the successful Windows OS
installation is a separate test.

Qt Quick/QML, mobile layouts, animated transitions and visual effects are not
part of the frontend design. The compact Win32 dialog structure maps naturally
to Widgets; the backend remains independent of widget classes.

Build-system decision, clarified 2026-09-14: CMake owns Linux configuration,
source/dependency selection, compilation, test registration and future install
targets. Dependencies compiled as part of the port use CMake targets too; the
Linux build must not require a separate Autotools, qmake or Meson build phase.
Ninja is a CMake-generated backend. Keep the Windows application on its existing
build systems, registering shared-source changes in their current manifests.
Do not migrate the Windows application to CMake or make either application's
build depend on the other platform's build files.

## Current baseline

The local fork is at `/home/marius/proj/lufus/rufus`, nested inside the older
Lufus checkout. It is an independent Git repository, not a submodule. Run
Rufus Git operations inside that directory.

| Item | Status |
| --- | --- |
| GitHub fork and remotes | Created; `origin` is `mcirsta/rufus`, `upstream` is `pbatard/rufus`. |
| Upstream source baseline | `18ae93bf37f94764d77216cff4168e0549a14c4d`. |
| Working branch | `codex/linux-qt6-reporting`. |
| Independent Linux CMake build | Implemented in [linux/CMakeLists.txt](../linux/CMakeLists.txt). |
| Reporting component | Implemented in [RufusJob](../linux/qt/rufus_job.h) and its C++ implementation. |
| C reporting declarations | Implemented in [rufus_reporting.h](../linux/platform/rufus_reporting.h). |
| Reporting verification | Eight behavioral test cases passed with Qt 6.11.2, including delivery to real Widgets through queued connections and recovery after an exception. |
| Reporting style alignment | Complete: named worker `execute()` implementation, named test receivers, no lambdas or `std::function`, and next-line braces. |
| Upstream compilation probes | Sixteen C translation units compiled unchanged; a seventeenth needed a compiler-supplied allocation header. These were object-compilation probes. |
| Main Widgets application | CP1 implemented: Win32-based form, image/device selection state, advanced sections and log window. |
| Widgets verification | 20 functional cases/data rows plus a separate 2× large-font layout check. Synthetic devices remain test-only. |
| Linux CI | Additive workflow prepared for Ubuntu 22.04/24.04; not yet executed on GitHub. |
| Linux device discovery | CP2 implemented: asynchronous read-only UDisks2 reader, hotplug and failure handling, private-bus tests. |
| Shared image policy | CP3b extracts the neutral report and image policy. CP10a feeds shared boot-marker detection into that unchanged policy during SELECT, distinguishing a DD candidate from an image requiring DD. Native Windows verification pending. |
| ISOHybrid mode dialog | CP10b shares the original ordered mode choices between Windows and Qt, with ISO as the default, conditional ISO → ESP, DD and cancellation. CP10v opens it from START, hands explicit DD to the raw writer and disables the unimplemented ESP mode with an explanation. |
| Native ISO/UDF extraction | CP10c shares traversal and file readers; CP10g/CP10h preserve Rufus filename and collision behavior. Mounted-FAT32 tests cover names/bytes, replacements, ENOSPC and EROFS. CP10n integrates extraction into the authorized helper; CP10o boots its generated EFI tree and CP10p boots Alpine standard 3.22.5. CP10q shares root Syslinux config generation with Windows; CP10r/CP10s/CP10t add shared config rules and Linux text-file handling. CP10v connects ordinary UEFI ISO creation to START; broader compatibility remains. |
| Native FAT32 preparation | CP10d formats an explicit region of a retained regular file with mkfs.fat, then flushes and checks it using Rufus's FAT reader. CP10e handles sector-aligned boundaries, including odd sector counts. CP10m reuses that implementation on the native partition descriptor, with explicit disk-claim release and reacquisition inside one authorized helper. CP10v uses it from the ordinary UEFI ISO workflow. |
| Syslinux MBR creation | CP12a–CP12m select a compatible native installer/payload, retain shared Rufus layout/config/progress rules and connect the existing one-helper lifecycle to Widgets. CP12o qualifies installed-runtime startup and live Widgets authorization/cancellation/creation. CP12p verifies one Alpine MBR/FAT32 USB through independent readback and both BIOS and UEFI boots; regular EFI files enable the corresponding firmware label. |
| Shared format options | CP10w shares UTF-16 volume-label normalization and FAT32 allowed/default cluster sizes with Windows; CP11b also shares NTFS cluster rules. Qt confirms the effective label and offers the supported FAT32 subset. NTFS selection is still pending. |
| Native NTFS formatting | CP11b invokes mkntfs with explicit geometry, cluster size and Unicode label, reusing FAT's process supervision and Rufus's boot-sector definition. CP11d retains the disk claim during formatting. CP11f shares extraction/cleanup, preferring the new kernel NTFS driver with NTFS-3G fallback. |
| GPT preparation | CP10e/CP11a share Rufus's main/trailing partition arithmetic. CP11c qualifies bounded file payload writes and NTFS firmware handoff. CP11d checks both native partitions and formats NTFS. CP11e writes/verifies the embedded boot payload through the retained device descriptor; CP11f adds ISO extraction in that same helper. |
| Preparation helper | CP10l executes one GPT/FAT32 ordinary-file request through a Qt process controller. CP10m adds one authorized device operation; CP10n extends it through source preflight, private FAT mounting, existing ISO extraction and flush/unmount cleanup. EOF cancels the job, including on caller death. File requests cannot run elevated. CP10o qualifies one-prompt creation and UEFI boot for a generated EFI tree; CP10v connects it to application START. |
| Shared file classification | CP3c1 implemented: file rules separated from extraction; integrated into shared ISO9660/UDF traversal and the Qt job through CP3d/CP3e. |
| Shared embedded EFI inspection | CP3f1/CP3f2 implemented: ISO9660 and UDF use Rufus's existing FAT reader, with bounded reads, generated FAT/fragmented-UDF fixtures and Qt functional tests. |
| Shared WinPE options | CP3g1 implemented: bounded SIF reads, original directory priority and `/minint` decision, with explicit known/unavailable results. |
| Native WIM metadata | CP3g2/CP3g3c implemented: shared archive/version queries work on ordinary files and selected ISO/UDF handles. SELECT uses explicit known/unknown/failed states, cancellation and generated WIM tests. |
| Shared ISO/UDF file ranges | CP3g3a implemented: EFI and WIM analysis use one concrete reader with fixed 32 KiB caching, arbitrary offsets and fragmented/selected views. |
| Upstream WIM reader extension | CP3g3b–CP3g3d implemented: the unchanged small patch is qualified against upstream release 1.14.5 and supplies the application provider. System wimlib is an optional independent test comparator. |
| WIM file extraction and PE architecture | CP3g4a implements native extraction of one file up to 16 MiB and shared bounded PE recognition, with generated-file and ownership tests. |
| Standalone WIM/ESD source | CP3g4b shares Rufus's signature check and qualifies native source ownership/WIM lookup. SELECT integration is deferred while the raw writing workflow is completed. |
| Linux media backend | Native raw copy/flush/read-back verification has regular-file and authorized block-descriptor entry points (CP5). CP8b qualifies native guest authorization, exclusivity, cancellation and unplug handling; CP9b adds reused-path reattachment and real Linux image writes. |
| Raw planning and authorization | CP4/CP7 components retain the source, validate a narrow `.img` plan and acquire/recheck a UDisks descriptor. Private D-Bus tests cover error contracts; CP8b verifies a native KDE/polkit prompt in the USB guest while the GUI remains unprivileged. |
| Raw writing in Widgets | CP8a connects SELECT, confirmation, authorization, the worker, progress, cancellation and verified completion. Offscreen tests exercise the full path with disposable files and simulated kernel facts. CP8b passes native USB qualification and enables the narrow raw `.img` workflow in the application. |
| Bootable-media validation | CP9 passes for the generated BIOS/FAT16 fixture. CP9b boots official OpenWrt BIOS/UEFI raw images. CP10o boots a generated EFI ISO tree and checks missing-payload failure. CP10p prepares/extracts official Alpine standard 3.22.5 to GPT/FAT32 and boots it through OVMF, checking release, USB payload and modloop. Alpine's embedded label search emits a nonfatal warning. Broader ISO compatibility, OS installation and Secure Boot remain unqualified. |

The reporting, image-analysis and raw-file tests establish their implemented
boundaries. The first application-to-device workflow now passes native USB guest
tests and boots both its generated BIOS fixture and real Linux BIOS/UEFI images.
This is a narrow raw-image path,
not full Rufus media or operating-system coverage.
Shared Rufus policy is exercised by Linux tests; broader policy application to
media choices and native Windows validation remain work for later checkpoints.

The current CMake floor is 3.21, the C++ floor is C++17, and the declared Qt6
floor is 6.2. Preserve those until a concrete requirement justifies a change;
actual compatibility with the declared minimum still needs CI coverage.

## Architecture and ownership

Use the following structure as the port develops. The reporting/build files,
Widgets shell, selection controller, UDisks2 clients, raw operation and their
GUI integration exist and pass simulated-storage and native USB guest tests.
The raw `.img` workflow is enabled and qualified with real OpenWrt BIOS/UEFI
images. CP10a adds ISOHybrid candidate detection and the existing Rufus policy
to SELECT. CP10b adds shared ordered mode choices and the Qt dialog component.
CP10c adds ordinary ISO/UDF tree extraction into private empty directories.
Scanning and extraction use the same synchronous C traversal; Linux supplies
descriptor-relative output creation and copying. No Qt object participates in
that internal traversal. CP10d adds native FAT32 formatting of a specified image
region with cancellation and independent filesystem checks. It accepts explicit
settings; it does not select different cluster sizes or silently normalize labels.
CP10e creates one ordinary GPT main partition and formats its exact recorded
extent. It shares placement arithmetic with Windows; table creation uses
libfdisk on Linux and the existing Windows disk APIs. CP10f qualifies the existing
tree extractor on mounted FAT32 inside a small disposable USB guest, checking
real filesystem names/content and cleanup with independent mtools/fsck readback.
No production-code change was needed for this qualification.

CP10g moves the filename substitution rule from `src/iso.c` into the existing
shared `image_file` module. Windows retains allocation, its unchanged-name flag
and drive-prefix handling. Linux converts only its output-path copy, including
parent directories and failed-file cleanup; readers retain original source names.
The exact six substitutions remain unchanged, with no additional filename policy
or per-entry allocation. Component, ISO/UDF and mounted-FAT32 tests cover the rule.

CP10h follows the Windows `CREATE_ALWAYS`/`FILE_OVERWRITE_IF` behavior for ordinary
files: the later source entry supplies the final bytes. Existing directories are
reused, including when substitutions make two source names identical. Linux first
tries exclusive creation; a collision opens the existing inode without following
symlinks and verifies a singly linked regular file before truncating it. Directory
reuse verifies a real directory. Filesystem-native aliases, such as FAT case
folding, determine collisions; no Linux filename index or custom case table is
introduced. The private target must still be empty at entry. Once truncation
succeeds, failure removes the incomplete replacement with no rollback. Processed
file/directory and byte counts include colliding source entries.

CP10i qualifies the existing GPT adapter on exclusive emulated USB descriptors.
Block requests supply the accepted device number, with capacity, 512-byte logical
sectors and read-only state checked natively before writing and after flushing.
Regular-file callers remain explicit: a zero device number rejects block nodes.
The adapter keeps the original libfdisk path and shared placement arithmetic;
it neither refreshes the kernel partition table nor handles authorization/mounts.

The same guest proves a formatter constraint: dosfstools 4.2 reopens
`/proc/self/fd/N` with `O_EXCL`, which fails with EBUSY while the retained exclusive
descriptor exists. `-I` does not change that open. Keep the FAT32 adapter restricted
to regular files until the production job owns the explicit handoff. Do not release the
claim and retry by pathname as an implicit fallback.

**Revised direction after discussion:** invoke native CLI formatters directly
with the requested options. Exclusive ownership may change between job phases:
release our descriptor for the formatter, let it obtain its own access, then
reacquire and revalidate the target before continuing. Uninterrupted ownership
is not a requirement. Do not automatically bypass a busy-device refusal. Keep
mount/device-use checks and handle a changed or disconnected target explicitly.
UDisks `Block.Format` and formatter patches are alternatives only if a specific
need remains; argument passthrough alone does not justify another service layer.

Authorize one small helper for the whole creation job. It keeps the privilege
needed to launch each native formatter and perform the remaining protected steps;
changing descriptors does not trigger a new authorization request. CP10j proves
the CLI handoff on GPT/FAT32 in the guest. CP10k qualifies one `pkexec` launch
from an ordinary-user Qt test through the real KDE authentication agent. One
authorization covers the existing GPT/formatter/mount/readback/cleanup tests;
the test policy uses `auth_admin`, with no cached grant. Dismissal and missing-agent
requests never enter the helper or retry authorization. Production integration
continues in CP10l: one bounded JSON request and phase/result messages use stdin
and stdout; EOF cancels the worker, including on caller disconnect. The first
operation uses the existing ordinary-file adapters and refuses elevated execution.
CP10m adds a distinct native operation through the same connection. The helper
checks fresh UDisks identity/use and the kernel disk sequence before writes and
around the formatter handoff. It retains a non-exclusive partition descriptor,
releases the disk claim for the CLI, then reacquires and verifies both GPT copies.
Cancellation before authorization stops pkexec; after elevation, pipe EOF reaches
the helper's cleanup path. There is no second launch or authorization retry.
CP10n extends this concrete job through a private mount and the existing extractor.
It accepts caller-owned, owner-readable regular ISO sources for this first path,
rejects final-component symlinks, retains the source descriptor and verifies the
accepted device/inode/size/mtime/ctime before writes and after copying. The same
extractor supplies preflight, including FAT32's per-file limit; source-on-target
checks use the shared UDisks logic. Flush and unmount failures remain visible even
when cancellation is also requested. Broader source permission/descriptor handoff
support is separate work; avoid a general RPC framework.
CP10o qualifies the existing GPT Basic Data/FAT32 layout on x86-64 OVMF with the
stock systemd UEFI loader and a kernel/initramfs in the copied tree. The EFI loader
uses paths on that filesystem; it needs no installer, label/UUID rewrite or BIOS
boot sectors. This generated ISO has no optical boot catalog and is not a tested
distribution ISO. Its successful boot does not make an arbitrary EFI-marked ISO
eligible: real images can require label/configuration fixes or native installers.

CP10p tests official Alpine standard 3.22.5 x86-64 on a 512 MiB USB, keeping the
EFI loader and boot configurations unchanged. Its on-disk GRUB menu uses paths;
its EFI executable also embeds a search for the original ISO label. With the
different FAT label, that search warns but the loader still boots from the USB
on OVMF. The live system verifies the original boot payload and mounted modloop.
This is a recorded compatibility limit, not a reason to add a binary patch.
Rufus's virtual El Torito entries are preserved and independently verified too.

CP10q moves root `syslinux.cfg` decisions and content into `image_file.c`, called
by both Windows and Linux. Preserve an existing config, or back it up as
`syslinux.org` when Rufus's shared scanner selects an override; create the same
CONFIG/APPEND redirect. Linux uses descriptor-relative file operations in the
existing extraction job. Its deliberate difference is to stop on backup failure,
preserving the original and any existing backup, where Windows ignores the rename
error and continues overwriting the original. This prevents data loss and is not
an implementation shortcut. No native Syslinux installer is selected or run.

CP10r extracts `SplitConfigToken` from `parser.c` into `config_text.c`. Windows
keeps its encoding/file handling. Linux's `ReplaceConfigTokenAt` uses that same
matching loop, Qt's Unicode converters and a checked temporary file plus atomic
rename. The adapter is tested independently and on the guest FAT driver;
CP10s connects it to ISO extraction. It preserves the original on pre-commit
errors and leaves an unmatched file byte-for-byte unchanged. It accepts UTF-8
(optional BOM) and UTF-16LE with BOM, rejects malformed/binary text, and limits
whole-file edits to 8 MiB of input/output to bound memory. Changes produce LF and
remove actual CR characters, preserving Unicode characters such as U+010D that
Windows's low-byte filter would remove. These differences prevent corruption or
unbounded allocation; they are explicit constraints, not silent fallback behavior.

CP10s moves `replace_char` unchanged out of the Windows parser and shares the
ordered config patch list in `config_text.c`. Both Windows and Linux use the same
seven label tokens, followed by the optional Red Hat `inst.stage2` → `inst.repo`
passes. Rufus's shared image policy supplies Red Hat detection; the original full
image filename supplies the case-sensitive `netinst` exception. Descriptor paths
remain exclusively for reading the retained image. Linux uses the actual requested
and verified FAT label, and the shared file classifier decides which configs apply.

Config text, supported encoding, the 8 MiB bound, replacement expansion and the
filesystem's final per-file size limit are checked during source preflight before
partitioning. Extraction repeats preflight and patches each completed config before
continuing, retaining source byte totals separately from config edits. It preserves
each pass's chunking/encoding round trip but commits the final result once, so a
later failure leaves the original config rather than partially applied fixes.
This is an intentional error-handling difference, not a change to successful rules.
The existing helper performs all steps with the same single authorization.

CP10t shares FreeNAS's `set` volume-reference formatting and EFI Syslinux config
selection/naming with Windows. GRUB files get the FreeNAS pass after the label and
Red Hat passes, including when labels are equal: the filesystem/path still changes
from `cd9660:/dev/iso9660/` to `msdosfs:/dev/msdosfs/`. These references retain literal
spaces. The same preflight checks include this pass before any GPT writes.

After label patching, `/EFI/BOOT/isolinux.cfg` is copied under its shared Syslinux
name only when the scan found no EFI `syslinux.cfg`. Preserve the original suffix's
case and keep an existing ordinary destination. Linux uses retained descriptors,
the existing 1 MiB transfer buffer and exclusive creation. Unlike Windows's ignored
copy failure, other failures stop the job and remove an incomplete new copy, keeping
the source config. A missing/partial config must not be reported as successful USB
creation. These changes stay in the same authorized helper and add no prompt.

The normal image classifications for GRUB and EFI isolinux configs are disjoint;
unit tests lock this down so FreeNAS text commits and EFI duplication retain their
original ordering. No native Syslinux installer is selected or run. Keep one layout
and retain CP10q's root config rule. Persistence remains disabled until its full
filesystem and configuration path exists. Alpine's embedded EFI-label warning is
still a recorded limit; do not patch that binary.

CP10v connects the GPT/FAT32 UEFI job to the existing ISO/DD dialog and
START, with explicit eligibility and its actual requested settings. Require a
complete supported tree, files fitting FAT32, a usable fallback EFI loader and
the necessary payload/configuration behavior. Detecting EFI alone is insufficient.
Do not hardcode the fixture's filename/hash into application eligibility, add a
general bootloader framework or bypass an unknown native installer version.
The dialog keeps Rufus's ISO default and DD choice throughout these steps.

```text
rufus/
  src/                         Rufus code, shared media modules and bundled libraries
  res/                         original resources and boot assets
  docs/
    linux-port-plan.md         this plan
    testing-and-checkpoints.md test strategy, checkpoint status and exit criteria
    linux-win32-ui.md          original control mapping and fidelity requirements
  tests/image_policy/         portable C tests, also runnable natively on Windows
  linux/
    CMakeLists.txt             independent Linux build
    cmake/                    explicit source and dependency selection
    qt/                       Widgets UI, controller, models and reporting job
    media/                    adapters for Rufus analysis and write planning
    platform/                 Linux service, tool and I/O implementations
    tests/                    component tests and media integration fixtures
```

The Widgets layer collects choices and presents state. A C++ controller owns
the device selection, image report, immutable write plan and active job. Reused
Rufus code supplies the media-specific behavior. Linux adapters perform the
platform operations requested by that plan.

Keep Qt out of shared C algorithms. Qt Core is appropriate in the Linux
integration layer; Qt Widgets belongs in the frontend. The proposed `media/`
directory holds integration and necessary orchestration, not a second copied
Rufus engine.

### Frontend and job lifecycle

Follow the [original control mapping](linux-win32-ui.md), including the Boot
selection combo, paired partition/target and filesystem/cluster fields, section
headings, toolbar, advanced sections and START/CLOSE buttons. Keep unavailable
operations disabled and image-dependent rows hidden until supported. Present
analysis through the corresponding Win32 controls and status/log areas instead
of introducing a different layout. Show the specific device and planned changes
before a destructive operation starts.

Use a model for devices, identified by stable metadata rather than the current
combo-box index. Snapshot the selected device and all options into a write
plan. A hotplug event or changed image must invalidate an obsolete plan.

The controller should distinguish at least these states:

```text
Idle -> Analyzing -> Ready -> Authorizing -> Preparing -> Writing -> Verifying -> Completed
```

An active operation can also reach Failed or Cancelled. Authorization denial
or cancellation before preparation returns to an editable Ready state with
the selection preserved and a clear explanation.

Refine the states around actual operations; do not build a general workflow
framework. Prevent conflicting actions while a job is active. A job controller
must outlive the worker even when a log window closes. Define application-exit
behavior so a running operation reaches an orderly stop before teardown.

### Reporting contract

Extend the existing adapter rather than introducing another reporting system:

- `uprintf()` and `uprintfs()` become owned `QString` log messages.
- `UpdateProgress()` retains its operation-local percentage or discrete-step
  meaning.
- `_UpdateProgressWithInfo()` retains operation IDs, message IDs, 64-bit values
  and control events. Initialization payloads are not always byte counts.
- Typed Qt signals carry these values to the GUI thread. The controller maps
  them to visible phases, status text and whole-job progress.

The current job uses scoped thread-local routing for context-free C calls and
a process-wide guard against overlapping legacy operations. Additional library
threads do not inherit that routing. When a library already supplies a callback
and context, use a small adapter for that API and manage its lifetime explicitly.
Do not expose widget pointers to C code or add a custom callback framework to
upstream callers.

Keep progress throttling, control-event order and final report flushing intact.
The existing `finished()` signal means termination, including failure; it must
not be treated as proof of successful media creation. C++ operation wrappers
must check C/library return codes and report failures explicitly.

CP3d connects read-only scan cancellation to the worker thread
interruption flag through the direct C query. Continue to use cooperative
checks in work loops and supported library callbacks for later operations. A queued cancel slot
cannot interrupt a worker blocked inside a synchronous C call. Define safe
cancellation boundaries for each formatting or writing operation and its
cleanup before enabling Cancel for that operation.

## Reuse strategy

For every candidate, record the upstream revision, the required behavior,
platform dependencies, selected implementation and verification. Distinguish
unchanged source reuse from adapted code and replacement of a platform service.

| Component | Planned treatment | Main work still required |
| --- | --- | --- |
| `src/cregex_*.c` | Compiled unchanged by the shared image-policy tests. | Native Windows CI and later Linux scan integration. |
| `src/syslinux/libinstaller/` | Prefer a native Syslinux installer where version, payload, filesystem and sector requirements match. Retain bundled algorithms as a fallback. | Test native installation with matching modules on disposable images before committing to a Windows installer port. |
| `src/syslinux/libfat/` | Reuse where image inspection or an installer fallback actually requires it. | Avoid porting its Windows I/O solely for work covered by a native installer; validate any retained read/allocation boundary. |
| `src/ms-sys/` | Reuse the boot-sector routines; select a Linux alternative to Windows `file.c`. | Preserve the existing I/O contract, offsets and sector sizes; validate resulting boot records. |
| `src/iso.c` and image-report definitions | Preserve Rufus's recognition, extraction decisions and media workarounds. The report now lives in `src/image_report.h`. | Separate mixed GUI/media responsibilities; retain sensible platform API adapters. |
| `src/parser.c`, `src/xml.c`, `src/wue.c` | Share useful policy and transformations where practical. | Separate host settings and platform operations from media behavior. |
| `src/vhd.c` and WIM operations | Retain Rufus's WIM decisions and use Linux wimlib. | Audit bundled changes, API differences, progress and error behavior. |
| `src/libcdio/` and `src/bled/` | Evaluate bundled versus system implementations based on required behavior. | Audit Rufus changes and supported image formats before substitution. |
| Win32 UI, device and formatting code | Leave the Windows implementations in place. | Recover any mixed-in media policy and implement the Linux service bindings. |

Prefer direct source selection: a Linux target can compile the existing
`ms-sys` algorithm files with a Linux I/O file, while the Windows build retains
the original `file.c`. Link one implementation of each reporting/platform
symbol into a given executable.

The larger ISO module cannot be made portable merely by changing the source
list. Mixed GUI/media logic must be separated in focused, behavior-preserving
changes to Rufus itself. Windows retains its dialogs, controls and workflow;
the shared C code supplies facts and media decisions. Use small native API
adapters for remaining platform operations. Verify Windows behavior at each
shared boundary. Avoid a broad Win32 compatibility layer, generated production
source rewriting, duplicating the ISO engine, or scattered platform branches.

The [native reuse assessment](linux-native-reuse.md) records the Syslinux
installer boundary, planned native filesystem/wimlib operations, and why the
bundled ISO reader currently remains preferable to the installed one.

## Syslinux detection and installation selection — agreed 2026-09-13

Keep image facts separate from the bootloader selected for installation. A
Syslinux image can have an unknown version; preserve its presence, detected
version/build suffix and whether the selected version came from detection or a
fallback. CP3c7 now records presence explicitly in `has_syslinux`, with
`HAS_SYSLINUX` using that field; `sl_detected_version`/suffix remain separate
from the existing selected-version fields and `sl_version_inferred` flag.
Do not populate image facts with the host tool's version.

CP3c7 shares Rufus's existing selection rules, including its embedded-v4/v6 fallback
based on `ldlinux.c32` when detection fails. Record that choice as inferred;
unknown detection alone must not reject the image. Preserve the existing
conflicting-loader and build-suffix rules when separating that policy.

Linux then selects a native installer able to install the chosen payload with
the required filesystem and sector geometry. If the system tool cannot do so,
evaluate a matching native build or Rufus's bundled installer algorithms with
Linux I/O. An unrelated host loader/module set is not an implicit substitute.
Resolve an unavailable supported installation path before destructive work.

Detection/presence and selection policy have component and generated-image
coverage. CP12a qualifies one native 6.04-pre1 pair on disposable FAT32 volumes,
including root and subdirectory BIOS boots. Generated ISO checks retain an
unknown version as unknown while choosing the existing numeric v6 fallback.
CP12b connects that inferred choice to the exact qualified companion bytes and
boots a generated unknown-version ISO after shared extraction. It retains Rufus's
empty inferred suffix and leaves detection unknown. This is not a universal
compatibility claim; full image/module/layout checks and controlled helper
execution remain necessary before destructive operations.

## Linux operations and dependency choices

Use one selected implementation per operation initially. The alternatives
below are decisions to validate against required features, not a request to
build multiple interchangeable backends in advance.

| Operation | Preferred starting point | Decision or gap to resolve |
| --- | --- | --- |
| Device discovery and hotplug | UDisks2 over QtDBus. | Device identity, transport, mount state, eligibility and service availability. |
| Authorization and opening a target | One helper authorized for the complete creation job; the existing raw path already uses one UDisks2 descriptor request. | Retain privileges across native CLI phases; revalidate device identity and use when changing ownership. |
| Mounting and unmounting | UDisks2 filesystem services. | Coordinate active mounts, later automounts and cleanup of resources owned by the job. |
| Partition layout | Shared Rufus placement arithmetic and native libfdisk. | One GPT main partition is qualified on regular files and exclusive guest block descriptors. |
| Filesystem creation | Direct invocation of native `mkfs.fat`, `mkntfs`/`mkfs.ntfs` and `mkfs.exfat`. | Preserve Rufus's explicit options; qualify the installed tool's filesystem, label and cluster-size capabilities. Authorization remains separate. |
| WIM inspection, splitting and updating | Linux wimlib. | Preserve the Rufus choices and parameters; verify bundled-versus-system differences. |
| ISO/UDF access | Rufus's libcdio-based approach. | Choose bundled or system libraries after the behavioral audit. |
| Raw image writing | A small operation using an authorized descriptor and established POSIX I/O. | Partial I/O, size limits, cancellation, flush, verification and accurate error reporting. |
| Boot record/loader installation | Reused Rufus and bundled boot components where applicable. | Filesystem-specific placement, versions, offsets and actual boot validation. |
| Uncovered privileged operations | A narrowly scoped helper only when a concrete service gap is established. | Keep its interface operation-specific; do not run the GUI as root. |

For external tools, use structured arguments without a shell, check exit status
and diagnostics, and implement bounded cancellation/cleanup. Check required
capabilities before changing a target. Source analysis should use library file
access without requiring privileged mounts where the chosen library supports it.

The formatting decision is to keep direct control of native tool arguments for
FAT16, FAT32, NTFS and exFAT. Keep process handling small and use existing facilities
where they help; the C monotonic-millisecond helper can stay. Qualify
`ntfsprogs-plus` and current NTFS-3G utilities as NTFS providers, and exfatprogs
for exFAT, before relying on their options. Tool selection must preserve Rufus's
media decisions and remains separate from mounting-driver selection and
job authorization through polkit.

CP10e uses libfdisk (2.35+) for regular image files: its descriptor API and
explicit partition bounds avoid loop devices or privileges in this checkpoint.
CP10i extends the same adapter to a caller-owned exclusive block descriptor,
qualified only inside a disposable USB guest. Native GPT checks do not replace
the UDisks plan, authorization or device-use checks.
Keep the qualified libfdisk layout and native CLI formatting in the authorized
job helper. UDisks2 continues to supply discovery and the existing raw access
path. CLI ownership changes must not cause repeated privilege prompts.

Every destructive path must revalidate the target identity and capacity, ensure
the source does not reside on the target being erased, and handle disconnects
and I/O failures. These are responsibilities of the job/controller even when
the underlying operation is delegated to a system service.

## Privileges and desktop authentication

Some partitioning, formatting, mounting and device-access operations require
privileges. Keep the Widgets application running as the ordinary user and
delegate those operations to a small helper authorized for the complete USB
creation job. The existing raw workflow uses UDisks2 descriptor authorization;
discovery can continue through UDisks2. The session's authentication
agent supplies the password dialog and chooses the identity according to
system policy. This may be the current user, an administrator or root; some
authorized actions need no prompt. [UDisks2 authorization](https://storaged.org/doc/udisks2-api/latest/udisks-polkit-actions.html),
[polkit authentication agents](https://polkit.pages.freedesktop.org/polkit/polkit.8.html).

The application requests permission for a concrete operation. It must not
collect, store or log a root password, implement its own password dialog, or
feed passwords to `sudo`/`su`. An authorization prompt is separate from the
application's confirmation of which device will be erased.

**User requirement:** at most one authorization request per USB creation, before
any writes. One helper retains its privileged execution context through GPT,
formatting, mounting, bootloader installation, verification and cleanup. Releasing
device exclusivity for a CLI formatter does not release the helper's privileges.
Do not chain individually authenticated service operations or depend on polkit's
temporary authorization cache. The helper accepts the concrete creation plan;
it is not an unrestricted command runner or a root GUI.

### User flow

1. Analyze the source and prepare the write plan as the ordinary user.
2. Show the target identity and planned changes; the user chooses to proceed.
3. Enter Authorizing and request the complete creation job asynchronously, allowing
   the system authentication agent to interact. Keep the window responsive.
   Background discovery does not trigger prompts. The existing UDisks raw path
   still allows interaction for its one descriptor acquisition.
4. On success, revalidate the target after the possible authentication delay
   and continue only the authorized work in that helper. No subsequent job phase
   or cleanup operation requests fresh authentication. If the helper dies, fail
   the job and report the partial state; do not silently relaunch it with a prompt.
5. Treat cancellation and denial as expected outcomes. Preserve selections,
   stop before the unauthorized step, and explain what happened without an
   automatic retry loop. If earlier steps changed the device, report that
   partial state accurately and perform the cleanup still permitted.

Respect system policy through one explicit job authorization; do not install
blanket authorization bypasses. Set timeouts with human interaction in mind and
distinguish an unanswered prompt from an I/O failure. Denial/cancellation does
not start another authorization attempt within the same creation request.

### Portability and missing services

Use the same service protocol across supported Linux distributions and desktop
sessions. Validate GNOME and KDE Plasma, a minimal desktop with a polkit agent,
and both Wayland and X11 where supported. Different authentication dialog
appearances are expected. Include a distribution with a locked root account
so the design never assumes that a usable root password exists.

If the system bus, UDisks2, polkit or the session authentication agent is
unavailable, preserve read-only functionality and show an actionable dependency
or session-setup message for the protected operation. Do not silently launch
the entire GUI as root. A future explicit command-line mode can use standard
text authentication facilities; the desktop application must not unexpectedly
open a terminal to obtain a password.

The Widgets controller deals with authorization states and results. Linux
service details stay in the Linux implementation, selected by the build;
the existing Windows elevation/UI path remains independent. Qt does not
provide one universal root-password API, so portability comes from this small
boundary and native system mechanisms, not a generic authentication framework.

Use a small installed helper with narrowly defined job operations and system
authentication. Choose the simplest suitable polkit-authorized activation or
`pkexec` launch for the complete job; avoid a separate daemon if it adds no value.
The privileged side must validate the request itself, including target identity
and allowed operations. Elevate the helper only; do not provide arbitrary
command execution or pass GUI secrets through arguments or environment values.
[pkexec](https://polkit.pages.freedesktop.org/polkit/pkexec.1.html).

Authentication behavior is part of the Phase 3 acceptance criteria and the
packaging/dependency matrix. Tests must cover approval, denial, cancellation,
missing agent/service, helper failure and target removal during authentication.
Count authorization requests: at most one for the complete creation, including
long-running jobs, formatter failures, cancellation and cleanup. Credential-cache
expiry must not affect an already authorized helper. CP10k automates real KDE/X11
prompts only inside a disposable VM, using its public fixture password in the
separate native agent. Host automation must not obtain passwords. Other supported
desktop/session combinations still need native qualification.

The production launcher must disable `pkexec`'s internal text agent and avoid
automatic authorization retries. KDE's Escape action in CP10k returned generic
denial (127), not the separate dismissed status (126). Preserve whether our caller
requested cancellation and whether the helper actually started; do not classify
every 127 as user cancellation. The ordinary-user GUI also cannot rely on sending
signals to terminate a root helper. Give the helper a cancellation channel and
make it clean up after caller disconnect. Qualify both behaviors before connecting
the production job to START.

## Coding style and performance

Use `/home/marius/proj/allparts_new/src/` as the style reference for newly
written Linux code. The reviewed examples include `pricing/PriceCalculation.cc`,
`search/SubmittedSearch.cc`, `admin/DecimalInput.cc` and their headers: concrete
data types, named operations, explicit validation and readable control flow.
Some reference files contain lambdas; the user's explicit no-lambda decision
takes precedence. Preserve the established style of unchanged upstream files.

- **No lambdas in new production code or tests.** Use named free functions,
  member functions and Qt slots. Use typed signal-to-member connections rather
  than capture-based connection handlers.
- **Use templates only for a real, significant advantage.** Do not introduce
  generic job runners, policy classes, traits or template-based adapter
  frameworks. Existing Qt/STL containers, optional values, ownership utilities
  and typed `connect` remain reasonable where they provide clear correctness,
  lifetime or performance benefits. Do not replace them with handwritten
  infrastructure merely to avoid all library template use.
- **Prefer direct calls and concrete structures.** Use explicit job data and
  named worker entry points. Avoid `std::function`/type-erased task injection
  when a normal function or slot does the job. A plain function pointer plus
  context is acceptable when an existing C API actually requires that shape.
- **Keep functions small enough to understand as one useful operation.** Split
  at real responsibilities, error/ownership boundaries or worthwhile reuse.
  Do not impose an arbitrary line limit or fragment straight-line work into
  chains of three-line helpers. Keep closely related validation and action
  together when it makes the code easier to follow.
- **Use a few necessary components.** Widgets, a controller, job code and
  platform functions describe responsibilities; they do not mandate interfaces,
  factories, service layers or a class for every function. Add an abstraction
  only when it removes demonstrated complexity or duplication.
- **Make control flow and ownership visible.** Prefer ordinary loops and
  branches, descriptive names, straightforward headers and local implementation
  helpers. Follow the reference's brace-on-next-line style for new C++ code.
  Use RAII for actual resource lifetimes; avoid wrappers that only rename a
  call. Keep error handling explicit without a new generic result framework.
- **Name compound conditions.** Use local variables that explain the meaning of
  a check, such as `fits_partition`, `cluster_size_matches` or `sector_aligned`.
  Keep each branch focused on related checks; do not pack unrelated validations
  into a long `if` expression. Preserve short-circuit guards before dereferencing
  pointers, subtracting unsigned sizes or dividing. Report which validation
  failed where that helps diagnosis.
- **Name repeated values and their units.** Use constants for filesystem limits,
  sector/block sizes, layout fields and timeouts. Derive dependent values from
  those constants, including formatter arguments, rather than repeating numeric
  and string literals. Keep constants near their users and reuse existing
  definitions when their meaning matches; do not create an abstraction layer
  just to share constants. Ordinary zero/one checks need no symbolic names.
  Apply these rules to our test drivers too: name fault modes and operational
  limits, while keeping format reference values independent of production.
- **Keep useful small C helpers.** `MonotonicMilliseconds` can remain a direct
  wrapper over the native clock. Short helpers are acceptable when they clarify
  a repeated operation; brevity alone is neither a reason to remove them nor
  to create a chain of trivial wrappers.
- **Preserve performance in the work that matters.** Keep UI updates and
  authorization requests out of per-block loops. Use reusable bounded buffers,
  avoid repeated allocation and unnecessary copies, and retain 64-bit sizes
  and offsets. Coalesce progress and batch high-volume logging where needed.
  Use owned queued data when thread lifetime requires it.
- **Measure before increasing complexity.** Track analysis time, write and
  verification throughput, memory use and GUI responsiveness on representative
  images. Compare with the underlying library/tool path where useful. Do not
  trade correct I/O, flushes or error handling for apparent benchmark gains.

The reporting prototype now follows these rules. Concrete workers override
`RufusJob::execute()`; `run()` retains the shared lifecycle and serialization.
This small base avoids duplicating those guarantees in every worker, with one
virtual call per operation. Tests use named receivers and a concrete test job.
Library optional values and RAII remain for presence and lifetime correctness.

## Implementation phases

### Phase 0 — Repository and reporting foundation

Status: complete at checkpoint CP0; foundation and style alignment tested.

The fork, remotes, independent CMake build, C-linkage reporting implementation,
typed Qt signals and eight behavioral test cases are in place. Retain this
baseline as the first reviewable change. No application/media capability is
implied by completion of this phase. Preserve these tests as the port grows.

### Phase 1 — Widgets shell and read-only device discovery

Depends on Phase 0, including its coding-style alignment.
CP1's Widgets shell and CP2's read-only discovery are complete. Media jobs and
their controller integration remain later checkpoints.

1. Add the Widgets application target and a compact main window using standard
   controls and layouts matching the original Win32 GUI.
2. Add a controller and device model, with asynchronous UDisks2 discovery and
   hotplug updates. Display identity, capacity and relevant usage information.
3. Add image selection, a log view, explicit job states and dependency status.
4. Connect the existing reporting job to the controller. Exercise progress,
   failure and closing a log window with a clearly identified test operation
   implemented with named functions/slots.
5. Keep writing unavailable until the image, device and backend capability are
   known and a valid plan can be produced.

Completion criteria: the application builds and runs unprivileged; discovery
does not block the UI; selections remain stable across refreshes; unplugging a
device invalidates the selection; keyboard and scaled-layout checks pass; the
window has no apparently functional controls for unsupported operations.

### Phase 2 — Prove shared Rufus image analysis

Depends on Phase 0; integrate the result into the Phase 1 controller.

1. Inventory the exact dependencies of one useful ISO/UDF analysis path and
   select its library implementation.
2. Compile useful unchanged modules from `src/` through explicit Linux targets.
3. Separate mixed GUI/media code in small, tested upstream-visible changes.
   CP3b–CP3c8 have extracted post-scan policy, file rules, ISO9660 traversal,
   image-file buffer reading, Syslinux detection/selection, GRUB inspection,
   EFI banner diagnostics and label/size/content finalization. One shared
   ISO9660 metadata operation now returns explicit complete/partial/error/cancelled
   results. CP3e shares UDF traversal; CP3f1 shares ISO9660 embedded EFI/FAT
   inspection; CP3f2 adds bounded UDF access to that inspection. CP3g1 shares
   WinPE options. CP3g2 adds ordinary-WIM metadata with native wimlib and shared
   Windows version normalization. CP3g3a–CP3g3d add WIM metadata through the
   shared ISO/UDF reader, SELECT integration and qualification against upstream
   wimlib 1.14.5. CP3g4a adds bounded single-file extraction and shared PE
   architecture recognition. CP3g4b shares standalone WIM recognition and qualifies
   native source ownership and Windows-compatible WIM lookup. SELECT integration and Secure Boot remain separate
   increments; the [Windows-image source map](linux-windows-image-metadata.md)
   records their boundaries.
4. Expose an owned image report to the controller, retaining Rufus's relevant
   image classifications, boot markers, large-file handling and decisions.
5. Add image fixtures and expected reports, referencing the selected upstream
   behavior. Identify uncertainty explicitly instead of silently guessing.

Completion criteria: a real supported image can be analyzed on Linux through
a demonstrably shared Rufus path; the UI shows the resulting report; automated
checks cover the supported distinctions; reused files and adapters are recorded.
A generic byte-copy prototype alone does not satisfy this phase.

Resolve the architecture risk in this phase before committing to broad
extraction-mode development. If the proposed boundary requires excessive
Windows disruption, revise the boundary and document the tradeoff.

### Phase 3 — Write planning and raw-image jobs

Depends on Phases 1 and 2 for the integrated application. I/O component checks
can be developed against ordinary temporary files beforehand.

This is the active phase. The existing supported analysis is sufficient to begin;
completing every remaining Phase 2 image variant is not a prerequisite. CP5 now
provides the native copy/flush/verify component, implemented ahead of the CP4
planner because its descriptor-based operation is independently testable.
CP4/CP7 now add the retained source/target plan and native descriptor acquisition.
The first input is an uncompressed `.img`, with whole 512-byte sectors and Rufus's
existing `55 AA` acceptance marker. This does not qualify its bootability or add
general ISO/DD/container detection. Mounted, swap and known layered targets are
rejected; automatic unmounting is deferred. CP8a connects the plan to explicit
confirmation and a named Qt worker. CP8b qualifies that path with an emulated USB
target and native UDisks/polkit: exclusive access, removal, flush/read-back and
post-write discovery pass, and the application connection is enabled. CP9 boots
the verified generated BIOS fixture in a separate VM. CP9b also boots verified
OpenWrt BIOS/UEFI outputs to Linux userspace and rejects a stale plan after live
reattachment at the same path. The existing production code passes these added
checks; this qualification required only test-harness and documentation changes.
Next, reuse Rufus's existing detection for one Linux ISOHybrid/DD workflow,
with its Win32 mode-selection behavior and a representative boot check.

1. Build immutable write plans from image analysis, device capabilities and
   user options. Separate planning from execution.
2. Add target revalidation, explicit confirmation, the native authentication
   flow and operation ownership. Reject unsupported or inconsistent plans
   before modifying media. Revalidate after waiting for authorization.
3. Implement the raw-image path with bounded reads/writes, error propagation,
   cooperative cancellation and flush behavior.
4. Add verification of the written image range and accurate completion states.
5. Integrate progress into the Widgets controller without losing operation
   boundaries or treating worker termination as success.

Completion criteria: temporary-file tests verify exact output and failure
handling; source/target and capacity checks work; cancellation reaches a known
state; explicitly selected disposable-device tests pass before device writing
is advertised as supported. Authentication approval, cancellation, denial and
missing-agent/service cases have defined behavior; the GUI stays unprivileged
and never handles the password.

### Phase 4 — FAT32/UEFI extraction mode

Depends on shared analysis from Phase 2 and planning/platform ownership from
Phase 3.

1. Add a supported partition layout using the selected Linux facility.
2. Create FAT32 through existing Linux formatting facilities.
3. Integrate ISO/UDF extraction with Rufus's applicable media decisions,
   filename handling and configuration adjustments.
4. Add wimlib-based large-WIM splitting where required by the supported Windows
   media path, preserving the chosen Rufus behavior.
5. Implement the complete prepare/extract/finalize/verify sequence and failure
   cleanup for the supported image set.

Completion criteria: supported Linux and Windows installer fixtures produce
the expected files and metadata; applicable UEFI virtual-machine boot checks
reach the intended bootloader or installer; the supported-media matrix records
both capabilities and exclusions.

### Phase 5 — Broader Rufus boot and Windows-media support

Depends on a working Phase 4 path. Add features in independently reviewable
increments rather than coupling all of them to one release.

Candidate increments are NTFS/UEFI support using the appropriate boot assets,
BIOS/MBR and Syslinux paths, additional compressed-image formats and media
quirks, and Windows customization based on shared Rufus rules. Persistence
and further filesystems follow concrete user needs and verified behavior.

For each increment, identify reusable policy, Linux services, required boot
assets and platform gaps first. Preserve provenance and notices. Enable a
feature in the UI only when its job and validation exist.

Completion criteria per increment: its output is checked against expected
Rufus behavior, error/cancellation paths are exercised, and relevant boot tests
pass. Full Windows To Go parity, ISO download integration and every long-tail
Rufus feature are not initial-release requirements.

### Phase 6 — Packaging and upstream preparation

Start Linux CI with Phase 1 and extend it throughout development. Complete the
following before the first supported release or upstream proposal:

1. Document tested distributions, runtime dependencies, optional capabilities
   and any authorization requirements, including session authentication agents
   and packaging of a helper if one proves necessary.
2. Validate the declared minimum Qt version and the current development
   version. Start runtime support with the tested host architecture and expand
   only after the relevant portability and boot checks.
3. Provide installation, desktop integration and reproducible build directions.
4. Maintain a media/boot support matrix and known limitations.
5. Review the upstream diff for accidental formatting, generated files,
   duplicated code and unnecessary Windows build changes.
6. Separate additive Linux work from any small shared-code extraction. Supply
   Windows validation for each change that touches a shared implementation.
   Per the user's decision, that validation will happen later on real Windows;
   it is required for upstream readiness, not for continuing Linux checkpoints.

Completion criteria: the port builds and installs from documented dependencies,
Linux checks run independently, and a reviewer can identify the small shared
surface and who maintains the Linux-specific code. Keep release and support
responsibilities in the fork unless upstream explicitly chooses otherwise.

## Verification approach

Use tests that establish behavior at real integration boundaries:

| Boundary | Required evidence |
| --- | --- |
| C reporting to Qt | Preserve the existing tests for ownership, thread delivery, progress semantics, ordering and serialization. |
| Device/controller state | Hotplug, stale selections, service errors, active-job restrictions and orderly teardown. |
| Image analysis and planning | Small fixtures with expected image reports and operation choices, traceable to the selected Rufus revision. |
| Platform adapters | Operation-specific error, authorization, cleanup and capability checks. |
| Authentication | Native prompts on supported desktops; approval, denial, cancellation, missing services, and target changes while awaiting authorization. |
| Raw writes | Byte-for-byte temporary-file comparisons, short I/O/error handling and cancellation. |
| Extracted media | Expected layout, files, configuration changes, labels and large-file treatment. |
| Boot behavior | Virtual-machine boot checks for the supported BIOS/UEFI paths, followed by deliberate disposable-media integration checks. |
| Shared-source changes | Windows build and relevant behavior checks in addition to Linux coverage. |
| Coding style and performance | Named functions/slots, justified template use, useful function boundaries, and measured throughput, memory use and responsiveness. |

Use ordinary files and small fixtures by default. Keep destructive device
tests explicitly opt-in with a selected disposable target. A successful build
or copy operation is not a boot-compatibility result.

The [testing and checkpoint plan](testing-and-checkpoints.md) specifies
offscreen Widgets tests, private D-Bus service simulations, temporary-image I/O,
and file-backed USB storage inside QEMU guests. The latter exercises the real
Linux storage stack without a physical stick; boot checks remain a separate
gate. Each checkpoint adds its relevant tests alongside the feature.

Build and test the current reporting component from the Rufus repository root:

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure
```

Let Ninja choose its default parallelism. Add checks as functionality appears;
do not repeatedly rebuild unchanged components for documentation-only changes.

## Open implementation decisions

These need evidence during the relevant phase, rather than an upfront
framework or speculative dependency set:

- The first useful, minimally invasive boundary for sharing ISO/UDF analysis.
- Bundled versus system libcdio, decompression and wimlib behavior where Rufus
  carries modifications.
- Whether UDisks2 covers each required partitioning, formatting and descriptor
  operation, and precisely which gaps require another facility.
- Operation-specific cancellation, cleanup and whole-job progress mapping.
- Initial distribution, desktop/authentication-agent, architecture and
  supported-media coverage.

CP3's initial investigation is recorded in the
[image-analysis boundary discussion](linux-image-analysis-boundary.md), including
the user preference for sensible Linux adapters, required GUI/media separation
and a measured bundled/system reader difference. CP3b has extracted post-scan
policy and the shared report; CP3c1 separates per-file classification and tests
it through the bundled reader. CP3c2 adds shared ISO9660 tree traversal, original
string arrays and feature-detected native reader builds. Generated-image and
allocation-failure tests pass. CP3c3 shares image-file buffer reading and tests
exact contents, format preference and cleanup. CP3c4 adds Syslinux version
recognition while keeping native installer selection separate. CP3c5 shares
GRUB version/build and filesystem inspection, with explicit report/list arguments.
CP3c6 shares Shim/systemd-boot banner diagnostics; source inspection confirmed
that this routine has no PE-reader dependency. PE/signature/revocation analysis
belongs to the separate Secure Boot workflow. CP3c7 shares configuration
selection and Syslinux scan policy, preserving the agreed presence/detection/
installation-choice distinction. CP3c8 now shares label/size finalization and
content-stage orchestration, with explicit partial/error/cancelled outcomes and
no callback layer. CP3e now shares UDF traversal in the same scanner and
renames the coordinator to `AnalyzeIsoImage`. UDF-first tree/content routing is
consistent, with an explicit failure for an unreadable UDF root. Completion is
scoped to UDF/ISO9660 metadata; raw-image
detection, final media policy and installer eligibility remain later stages.
CP3d now connects this operation to SELECT through a concrete Qt job, keeping
incomplete checks explicit and START disabled. It preserves Win32's marquee
progress and CLOSE/CANCEL behavior and rejects stale queued reports using
selection request numbers. Twenty-one Qt functional cases/data rows and the full 26-test
suite pass normally and with sanitizers. Shared UDF/ISO9660 analysis now reaches
the application; full image-format and real distribution qualification remain
pending. Thirteen generated UDF fixtures, seventeen reader checks and the Qt
cases verify CP3e, using both native genisoimage and mkisofs. CP3f1 now shares
ISO9660 embedded EFI/FAT presence inspection using Rufus's existing reader and
32 KiB read-ahead. Twenty-seven generated fixtures cover FAT12/16/32, 4 KiB
sectors, partition/file bounds, failure and cancellation. Presence leaves Secure
Boot pending; completed absence and unreadable candidates have distinct results.
CP3f2 now adds bounded UDF access to the same inspection without companion-ISO
fallback, whole-image buffers or mounts. Sixteen UDF containers and 22 direct
range checks cover fragmented short/long extents, distinct filesystem views,
EOF and failures. Windows also selects the UDF view for this shared inspection;
native Windows execution remains deferred. CP3g1 now shares WinPE `txtsetup.sif`
inspection, with original-case paths and a 64 KiB limit enforced before allocation.
Twenty ISO/UDF fixtures and Qt tests distinguish known `/minint` options from
pending checks. CP3g2 now provides native wimlib metadata queries on ordinary
WIM files, with shared archive information and version normalization. Generated
WIMs cover multiple images, unknown/invalid properties, cancellation and failures.
CP3g3a now shares the existing EFI file-range reader and verifies WIM bytes in
ISO/UDF, with fixed caching, exact ranges, cancellation and fragmented extents.
The public native wimlib API has no arbitrary-reader input hook. CP3g3b
provided the initial native build of upstream wimlib with a small input extension,
following the user's decision to avoid carrying the bundled Windows adaptations.
Five upstream files change (44 added and 5 removed lines), with a concrete reader
adapter; WIM/XML parsing remains upstream. Generated ISO/UDF metadata matches the
ordinary-file comparator, including pipable and fragmented inputs. See the
[prototype decision and validation](linux-wimlib-prototype.md). A dedicated
wimlib fork is an option for later maintenance, not required now. CP3g3c now uses
one extended upstream provider and connects qualified metadata to SELECT. The
worker retains the scan's selected filesystem, reads the first installation path
and WIM image 1, then closes the borrowed-reader chain before publishing. Failed
or unknown metadata stays partial without trying a later candidate; unrelated
WinPE/Secure Boot checks remain pending. The existing form/status/log handle the
results and stale selections cannot restore old Windows facts. CP3g3d qualifies
the unchanged extension against upstream release 1.14.5. The release's source
hashes, published archive checksum and explicit build version replace the old
comparison pin; no separate fork or adapter growth is required. Keep upgrades
explicit and rerun reader/metadata/Widgets tests before changing the pin.
CP3g4a now qualifies one-file extraction through the same provider and shares
Rufus's machine mapping and PE parser. Native wimlib queries and extracts the
selected regular file, at most 16 MiB, with private temporary output and explicit
cleanup/cancellation. SELECT integration must follow the original standalone
WIM/ESD branch; a boot-manager file inside an ISO's installation WIM must not be
used to infer the ISO's firmware bootability. CP3g4b qualifies standalone signature
classification, retained ordinary-file ownership and Windows-compatible WIM path
matching. Reject results when the retained file or pathname changes; upstream
may also reject a file unlinked before opening. An eight-byte signature identifies
a candidate, independently of its suffix; it does not establish a valid archive,
known Windows version or bootability. Later connect these stages to the existing
Qt worker/controller, preserving unknown/missing/failed states and cancellation.
This integration is paused while the first raw writing workflow is completed.
Avoid a default whole-WIM
copy into memory or temporary files. Archive
format version and Windows release/build remain separate facts. See the
[metadata boundary](linux-windows-image-metadata.md). El Torito load counts that do not define
the full image extent remain pending until a separate range decision is tested.
Windows metadata, Secure Boot, raw/hybrid detection and native installer capability
remain separate checkpoints. CP2's discovery uses a conservative initial
removable-USB filter; fixed USB support and write-time device-use checks still
need their own tested policy.

## Reference material

- [Current Linux component contracts and build instructions](../linux/README.md)
- [Testing strategy and checkpoint status](testing-and-checkpoints.md)
- [Reporting integration tests](../linux/tests/test_rufus_reporting.cpp)
- [Qt Widgets](https://doc.qt.io/qt-6/qtwidgets-index.html)
- [Qt worker objects and queued connections](https://doc.qt.io/qt-6/qthread.html#details)
- [UDisks2 API](https://storaged.org/doc/udisks2-api/latest/)
- [UDisks2 block operations](https://storaged.org/doc/udisks2-api/latest/gdbus-org.freedesktop.UDisks2.Block.html)
- [UDisks2 authorization checks](https://storaged.org/doc/udisks2-api/latest/udisks-polkit-actions.html)
- [UDisks2 interaction option](https://storaged.org/doc/udisks2-api/latest/udisks-std-options.html)
- [polkit authentication architecture](https://polkit.pages.freedesktop.org/polkit/polkit.8.html)
- [pkexec helper authorization](https://polkit.pages.freedesktop.org/polkit/pkexec.1.html)
- [wimlib](https://github.com/ebiggers/wimlib)
- [dosfstools](https://github.com/dosfstools/dosfstools)
- [exfatprogs](https://github.com/exfatprogs/exfatprogs)
- [util-linux](https://github.com/util-linux/util-linux)


**CP16q completed (2026-09-25), source-free SFD/FAT32 helper:**
The explicit `format-fat32-sfd-device-v1` operation reuses the existing format
job, whole-device preparation and qualified native formatter handoff. Preflight
rejects unsupported filesystems, source/boot options and conflicting layouts.
The client accepts only offset zero and the exact device extent for SFD, then
requires finalization and successful helper exit. Native failure/cancellation,
claim, sync and independent readback tests pass normally and under sanitizers;
the detailed record is in the testing log. CP16r connects selection, confirmation
and progress to this qualified operation.


**CP16r completed (2026-09-25), SFD/FAT32 in Qt:**
Non bootable offers MBR, GPT and Super Floppy Disk in Win32 order, with BIOS or
UEFI as target. MBR remains the initial default; explicit SFD choices survive
mode/device changes when supported, and disks above 2 TiB reset the preference
to GPT. The existing first-entry fallback applies when SFD is unavailable, without
losing the preference. A typed partition choice replaces the two-choice boolean.
SFD offers only FAT32 until other native routes are qualified; geometry uses
offset zero and exact device capacity. Confirmation names Super Floppy Disk,
filesystem, normalized label and cluster size. Existing progress, cancellation,
one-launch authorization and READY-after-sync/helper-exit rules apply.

**CP16s scope, FAT16 native/helper qualification.** Reuse the native
FAT16 formatter where its whole-device metadata rules permit. Qualify clearing,
claim handoff, FAT16 readback, cancellation and final sync on generated files and
simulated USB before exposing FAT16 in the existing SFD selector. Keep Windows
source unchanged unless a genuinely shared rule needs extraction. NTFS/exFAT
SFD and the independently planned CP17b mounted-target work remain in scope.


**CP16s completed (2026-09-25), FAT16 SFD native/helper qualification:**
The explicit `format-fat16-sfd-device-v1` operation uses the existing source-free
helper job and stock FAT16 formatter. SFD adapter checks select FAT16 geometry
and whole-device readback, preserving metadata clearing, claim handoff, empty
partition entries and final sync. File and simulated-USB tests cover readback,
refusals, failures and cancellation; details are in the testing record. The GUI
remains FAT32-only until CP16t adds FAT16 cluster/selection/default rules.

**CP16t scope, FAT16 SFD in Qt.** Extend the existing typed SFD choice to the
qualified FAT16 route, using whole-device cluster geometry and original Rufus
filesystem defaults. Test small FAT16-only media, FAT16/FAT32 overlap, the FAT16
capacity limit, remembered filesystem choices, confirmation, cancellation and
READY after helper exit. Reuse the current backend; no new privilege flow.
NTFS/exFAT SFD and CP17b mounted-target handling remain in scope.


**CP16t completed (2026-09-25), FAT16 SFD in Qt:**
The existing Super Floppy Disk choice offers FAT16/FAT32 according to native
whole-device geometry. It now works on small FAT16-only media. Original Rufus
filesystem order/defaults apply; valid existing/manual choices take precedence.
Explicitly selecting the already-displayed default records a preference, matching
Win32, while automatic fallback does not. Confirmation, label/cluster handling,
progress, cancellation and one-helper completion reuse the qualified FAT path.
The FAT16 choice disappears near 4 GiB if its native whole-volume cluster count
would be invalid; a smaller partition's geometry is not reused for SFD.

**CP16u scope, NTFS SFD native/helper qualification.** Check the configured native
mkntfs path against a whole-device extent, including its retained descriptor,
exclusive access, metadata and volume readback requirements. Reuse the current
source-free job and final sync; no bootloader payload, mount or extra privilege
prompt belongs in non-bootable formatting. Qualify file and simulated-USB failure,
cancellation and success paths before exposing NTFS under SFD. Keep exFAT SFD
and CP17b mounted-target handling in scope.


**CP16u completed (2026-09-25), NTFS SFD native/helper qualification:**
The source-free `format-ntfs-sfd-device-v1` job reuses the existing NTFS formatter,
SFD metadata preparation and helper lifecycle. The NTFS device adapter keeps the
exclusive whole-disk claim throughout native formatting and readback. Native
mkntfs requires `-F` for a whole disk; only the explicitly checked SFD extent
receives it, after stale kernel partitions are removed. Partition formatting
continues without force. Primary/backup boot sectors, MFT metadata, the full
extent and empty partition entries are checked before final whole-device sync.
No UEFI:NTFS payload or mount is involved. The GUI remains FAT16/FAT32 for SFD.

**CP16v scope, NTFS SFD in Qt.** Extend the existing SFD selection and cluster
filtering to the qualified NTFS whole-device extent. Preserve original Rufus
filesystem order/defaults, manual preferences, source-free confirmation and
READY only after final sync and helper exit. Exercise NTFS-only capacities,
FAT/NTFS overlap, unsupported geometry, cancellation and incorrect helper results.
Reuse this backend without a new authorization flow. ExFAT SFD and CP17b
mounted-target handling remain planned.


**CP16v completed (2026-09-25), NTFS SFD in Qt:**
Super Floppy Disk now offers NTFS alongside FAT16/FAT32 wherever the native
whole-volume geometry fits. Shared Rufus filesystem order/defaults and remembered
manual selections are unchanged. NTFS starts at 16 MiB; the original 256-TiB
limit and native cluster-count constraints still apply. These capacity boundaries
are selection/geometry checks, not large-device hardware qualifications.
The existing source-free job handles Unicode labels, confirmation, cancellation,
NTFS progress allocation and full READY after final sync and helper exit.

Fixed an explicit-selection mismatch found while enabling NTFS-only SFD targets:
on media larger than 2 TiB, refreshing device choices still defaults to GPT,
but selecting SFD now stays selected, matching Win32's target-only update.
The Linux MBR capacity limit is retained. No new backend or privilege flow.

**CP16w scope, exFAT SFD native/helper qualification.** Check whether the configured
native exFAT formatter can format a whole device with the requested cluster size,
label and zero partition offset. Reuse SFD clearing, the existing exFAT formatter
and claim handoff where their contracts apply. Qualify metadata/readback, stale
partition removal, cancellation and final sync on files and simulated USB before
exposing exFAT SFD in Qt. Keep CP17b mounted-target handling and the remaining
Windows parity backlog in scope.


**CP16w completed (2026-09-25), exFAT SFD native/helper qualification:**
The source-free `format-exfat-sfd-device-v1` job reuses SFD metadata clearing,
native exfatprogs and the existing authorized helper. Whole-device mode requires
zero offset and no sysfs partition marker; the adapter removes stale kernel
children and checks the full extent before handing over its claim. The native
formatter keeps the existing check-written/no-discard/no-partition-table options.
After reacquisition, the helper rechecks volume metadata and empty partition
entries, then syncs the whole disk. No force option, mount or extra authorization.
Normal/sanitizer file and simulated-device qualification is recorded in the
checkpoint log. The GUI remains FAT16/FAT32/NTFS for SFD at this checkpoint.

**CP16x scope, exFAT SFD in Qt.** Extend the existing SFD filesystem choices and
cluster filtering to the qualified exFAT whole-device extent. Preserve Rufus's
filesystem order/defaults and manual preferences, native exFAT label/metadata
constraints, source-free confirmation, progress and READY after final sync/helper
exit. Test geometry boundaries, fallback/restoration, cancellation and incorrect
helper results. Reuse the backend and one authorization flow. CP17b mounted-target
handling and the remaining Windows parity work stay planned.


**CP16x completed (2026-09-25), exFAT SFD in Qt:**
The existing Super Floppy Disk choice now offers FAT16/FAT32/NTFS/exFAT wherever
the native whole-device geometry fits. ExFAT reuses shared Rufus filesystem order,
default cluster thresholds and FAT-style label normalization, plus the qualified
source-free helper. The minimum whole volume is 16 MiB; default cluster sizes
change at 256 MiB and 32 GiB. Native metadata/cluster limits filter the offered
choices. Selection and arithmetic tests at large capacities are not hardware
creation qualifications. Confirmation, cancellation, original exFAT progress and
full READY after final sync/helper exit use the existing workflow. No backend,
privilege flow or Windows source changes were needed.

**Next: CP17b, mounted-target handling, beginning with the backend audit and a
bounded unmount step.** Follow the existing CP17b scope above: permit eligible
ordinary target volumes to be unmounted within one authorized job, retaining
source/system-device protections and refusal of busy, swap and layered devices.
Test refusal before writes, cancellation and one authorization on simulated
media before integrating the step into creation. The wider Windows parity
backlog, including remaining filesystems and full-format modes, remains active.


**CP17b1 completed (2026-09-25), mounted-target metadata audit.**
Separated an all-or-nothing, read-only `ListUDisksTargetMounts` from the existing
creation refusal. It validates the selected disk against the snapshot, collects
only whole-disk or explicitly related partition filesystem objects, and rejects
source-device matches, swap, encrypted/layered storage, protected children and
unknown/malformed mount state. Mounted candidates additionally require explicit
writable/non-system/non-ignored flags, a device number and valid absolute
NUL-terminated mountpoints. A root mount is rejected. No partial candidate list
escapes when another member fails validation. The existing creation check calls
this shared logic and still refuses every mounted target. No unmount, new
privilege request or helper/GUI behavior enabling mounted writes is included.

The audit identified the next integration constraints:

- Windows `FormatThread` obtains drive/volume handles and attempts dismount before
  writing (`src/format.c`, `src/drive.c::UnmountVolume`). Linux will use native
  unmount and exclusive block acquisition. It must fail if a mount/claim remains;
  Windows's best-effort dismount is not sufficient evidence to permit Linux writes.
- The preparation helper currently calls `unshare(CLONE_NEWNS)` and makes mounts
  recursively private before Qt starts threads. A later UDisks unmount in the
  service namespace would leave the helper's inherited mount alive. Qualification
  must cover both namespaces. Arrange unmount before inheriting private mounts,
  or explicitly account for both copies; do not merely relax the preflight gate
  or move `unshare` into a multithreaded Qt worker without qualifying its thread
  semantics and child launch paths (subsequently qualified in CP17b3 below).
- UDisks metadata is only a candidate precheck. Before any unmount, validate the
  actual kernel mount graph, system/source ancestry, sysfs holders and swap.
  `imageDevice == DeviceNumber` alone cannot locate sources through FUSE or other
  layered filesystems. A mountpoint-name blacklist cannot replace those checks.
  Refuse unresolved layouts. A busy filesystem must remain an error: no forced
  or lazy unmount, process killing, or swap deactivation.
- ISO/non-bootable jobs already have one authorized helper; DD uses a separate
  UDisks `OpenDevice` authorization and deliberately works without that helper
  (CP17a). Do not add an interactive UDisks unmount followed by a second prompt.
  Qualify the single-authorization ownership for mounted DD explicitly, while
  retaining helper-independent unmounted DD behavior.

**Next: CP17b2, native mounted-target checks and bounded unmount qualification.**
Use the candidate list only after fresh device/source validation. Test native
mount/source/holder refusal and busy/cancel/error behavior on simulated media,
with no writes or second authorization. Resolve namespace ordering before
connecting the step to creation. Existing mounted-target refusal remains until
that integration is qualified; the broader Windows feature backlog is unchanged.


**CP17b2 completed (2026-09-25), isolated native unmount backend.**
`target_mounts` uses util-linux libmount to read `/proc/self/mountinfo` and
`/proc/swaps`. A parser error invalidates the complete result; libmount's default
skip-broken-line behavior is disabled (see the upstream
[parser contract](https://github.com/util-linux/util-linux/blob/master/libmount/src/tab_parse.c)).
The adapter borrows a validated whole-disk descriptor, inventories its kernel
partitions, refuses holders/slaves, checks a retained source descriptor and
active block/file swap, and records mount IDs plus disk sequence. Supported
mounts are direct vfat/exFAT/ext2/ext3/ext4/kernel NTFS filesystems. Nested mounts,
bind/subtree aliases, multiple mounts of one filesystem, known system mounts,
and the helper's executable/current-directory filesystem are refused.

The root-only operation rechecks the complete remaining inventory before each
unmount, pins a root-owned non-group/world-writable parent directory, checks the
leaf's mount ID/device/type with statx, and uses native `umount2` with NOFOLLOW.
No force, lazy detach, recursive unmount, shell, process killing, remount or
additional authorization. Busy/error returns immediately; cancellation is checked
before and after native work and reports how many volumes have already unmounted.
There is one syscall attempt per expected volume and no retry loop. This is not
a hard deadline: a kernel unmount in progress may synchronize I/O and must finish
before cleanup/completion. Errors/cancellation do not remount earlier volumes.

Normal and ASan/UBSan disposable-USB guests pass 27 cases, including actual open
file EBUSY, device-mapper holders/slaves, active swap, protected mounts, source on
target, malformed mountinfo, stale identity, syscall failure, partial cancellation,
and successful two-volume unmount with an independent source retained. Both
filesystems pass independent host fsck afterward. A forked private namespace
reproduces the CP17b1 issue: unmounting in the original namespace leaves the child
copy alive and O_EXCL fails; removing that copy and exiting the child makes the
claim available. No host mounts/devices or Windows customization tests are used.

**Remaining boundary / next CP17b3.** This native adapter is not yet linked into
the creation helper or used to relax the GUI refusal. Before doing so:

- Match the native inventory exactly to fresh UDisks candidates and revalidate
  original source/device identity. Check the authenticated caller's home and
  other protected caller paths, in addition to the native system anchors.
- Resolve namespace ordering in the single authorized job; an empty inventory
  in one namespace is insufficient. Acquire and retain the existing O_EXCL claim
  after all relevant mounts disappear, before any preparation writes.
- Preserve one authorization for ISO/non-bootable jobs and mounted DD; preserve
  helper-independent unmounted DD. Qualify launch/cancellation/failure before
  exposing mounted creation. No real prompt qualification is claimed by CP17b2.
- Initially refuse unresolved FUSE, overlay, loop-backed and multi-device source
  locations. Native target unmount supports direct block filesystems only;
  NTFS-3G/FUSE-backed targets need explicit UDisks/native mapping and supervision
  qualification. They remain in scope. Untrusted mountpoint parents also fail.

The library is intentionally a small native adapter, not a replacement mount or
authorization service. CMake adds the system libmount dependency; Windows is
unchanged. Qualified guest disks/trees are removed, retaining compact evidence.

**CP17b3 completed (2026-09-25), worker mount-namespace lifetime.**
The preparation helper now creates its private mount namespace in the worker,
after source validation and exclusive target acquisition, before preparation
writes. It no longer copies desktop mounts before starting Qt. Native inventory
reads `/proc/thread-self/mountinfo`, so it sees the calling worker's mounts rather
than the main thread's view. Namespace setup errors return through the existing
job result before writes, within the same authorized helper.

This supersedes CP17b1's conservative restriction on worker isolation with a
qualified implementation. Linux permits `CLONE_NEWNS` for an individual thread
and also unshares its filesystem attributes; it does not require unsharing the
descriptor table (see [unshare(2)](https://man7.org/linux/man-pages/man2/unshare.2.html)).
Qt/event-loop threads retain their original namespace. The worker's QProcess and
posix_spawn children inherit its private namespace. Tests start with shared mount
propagation, verify isolation from the main thread, and require explicit cleanup
before worker exit. Complete helper creation, cancellation and boot tests pass
normally and under sanitizers; details are in the testing record.

**Next: CP17b4, fresh candidate matching and protected caller paths.**
Automatic unmounting remains disconnected and mounted creation remains refused.
Match fresh UDisks candidates to native mounts, revalidate retained source/device
identity and protect the authenticated caller's home before unmounting. Perform
eligible unmounts in the worker's original namespace, then acquire the exclusive
disk claim and use the qualified private namespace step. A mount surviving in
another namespace must still prevent acquisition. Qualify failures/cancellation
before exposing mounted creation in Qt. Preserve one authorization per complete
job, including mounted DD, and helper-independent unmounted DD. The unresolved
FUSE/layered-source and native mount-parent restrictions from CP17b2 remain.

**CP17b4 completed (2026-09-25), mount matching and retained caller home.**
`MatchUDisksTargetMounts` reuses the complete UDisks eligibility/candidate check,
then requires the original disk number/sequence and an exact one-to-one match
with the native inventory. Device numbers and raw mountpoint bytes must agree;
order may differ. Missing/extra volumes, aliases, duplicate device numbers,
mount IDs or paths, and stale/protected/source-device metadata fail. This is a
read-only comparison of supplied snapshots, not a new D-Bus request or permission.

`OpenNativeCallerHome` resolves an authenticated non-root UID through
[`getpwuid_r`](https://man7.org/linux/man-pages/man3/getpwnam.3.html), ignoring the
invoker's `HOME`. It retains the resolved directory with O_PATH/CLOEXEC, including
when the account home is a symlink. Missing/invalid accounts or homes fail.
Account buffer growth is bounded; cancellation is checked around the lookup/open,
but an in-progress NSS lookup can still block until the provider returns.
The existing native backing check now accepts either the retained source file
or caller-home directory. It refuses either on the target, and refuses unresolved
backing layouts. Home protection is rechecked before each unmount, alongside the
existing source, system anchors, swap and holder checks. A retained home descriptor
is required for caller-driven unmounting; the optional omitted descriptor exists
only for isolated native tests with no authenticated caller.

The metadata suite passes 85 cases and disposable USB guests pass 44 native
cases, both normally and under ASan/UBSan/leak detection. No creation or GUI gate
is relaxed by this checkpoint. Qualified guest disks/trees were removed.

**Next: CP17b5, preparation-helper unmount integration.**
Within the already authorized job, fetch fresh metadata from the validated UDisks
service owner, retain/validate the original source and caller home, and open a
read-only target descriptor with original identity/size/sequence checks. Match
the native inventory before any unmount, then use the native operation in the
worker's original namespace. Recheck service/device state and acquire O_EXCL
before isolating worker mounts and writing. Carry cancellation/partial unmount
errors through the existing job, never force/lazy unmount or prompt again.
Qualify service/mount/identity changes, home/source refusal, busy mounts and
cleanup using the complete helper before enabling Qt mounted creation. Preserve
existing unmounted-device behavior; do not impose mounted-layout restrictions
on already qualified unmounted jobs. Mounted DD's single-authorization route
still needs its own integration; unmounted DD must remain helper-independent.

**CP17b5 completed (2026-09-25), preparation-helper unmount integration.**
`PrepareDeviceTarget::open` now handles eligible mounted targets inside the
existing authorized helper. It validates the fresh UDisks service/device snapshot,
retains the authenticated caller's home, checks the retained source and a
read-only target descriptor, and matches the native inventory before unmounting.
The native adapter revalidates mounts, disk sequence, source/home, swap and
holders before each syscall. The helper logs the number actually unmounted,
including on partial failure/cancellation; it does not remount them.

After native unmount, the helper polls fresh metadata until UDisks reports no
remaining mounts, with a five-second polling deadline and the existing bounded
D-Bus calls. A call in progress may outlast the polling deadline. This tolerates
notification lag; it never retries unmount or ignores a busy mount. Source and
disk identity are rechecked before closing the read-only descriptor and acquiring
O_EXCL. A competing mount/claim still prevents writes. Only after exclusive
acquisition does the worker isolate its mount namespace and begin preparation.
The caller-home descriptor is retained through job cleanup, then closed before
completion. No new protocol, service, password handling or authorization request.

Already-unmounted targets keep their prior path and source support; the new
mounted-layout/home restrictions are not imposed on them. Qt preflight still
refuses mounted targets, and DD is unchanged. The helper path passes qualification
on whole-device FAT32 with the existing source-copy/boot regression; the testing
record lists the results and remaining exposure boundary.

**Next: CP17b6, mounted creation in the ordinary Qt workflow.**
Use eligible mount candidates during preflight/confirmation, leaving native
validation and unmount in the one authorized helper. Qualify partitioned and
source-backed mounted jobs alongside this integration, including cancellation
and source changes, before exposing them as supported GUI paths. Preserve Win32
confirmation/progress/completion behavior and clear failures for protected or
unsupported layouts. Mounted DD still needs its single-authorization route;
helper-independent unmounted DD remains unchanged. FUSE/NTFS-3G target mapping and
unresolved layered source/home backing remain refused and in scope for later work.

**CP17b6 completed (2026-09-26), mounted preparation in Qt.**
ISO/non-bootable/SFD preflight and post-confirmation recheck now accept eligible
mount candidates. The default selection check remains strict; root checks after
acquisition and DD are unchanged. Actual native validation/unmounting stays in
one authorized helper. The existing destructive confirmation/default Cancel,
progress, sync-before-READY and helper-exit ordering remain in place.

Fifteen new Qt rows cover ISO, partitioned formatting and SFD across success,
confirmation cancellation, protected metadata before/after confirmation and
permission denial. The existing mount-appearing-during-confirmation test now
expects the helper to handle an eligible mount; source/device changes still fail.
A disposable guest qualifies a mounted FAT32 partition with an independent ISO
and caller home: changed-source refusal, cancellation after unmount and complete
MBR/FAT32/Syslinux creation. Whole-disk refusal hashes, single helper launch,
cleanup, independent final readback/fsck and BIOS boot pass normally and under
ASan/UBSan. Completed disks and guest trees were removed.

This does not qualify every filesystem/desktop mount combination. FUSE/NTFS-3G
targets and unresolved layered source/home backing remain refused. The Windows
comparison also identifies an existing GUI gap: its extra multiple-partition
confirmation (MSG094/MSG093) remains in the GUI parity backlog; this checkpoint
does not claim complete confirmation-dialog parity.

**Next: CP17b7, mounted DD within one authorization.**
Choose a root-owned raw-write job that reuses the current writer and qualified
native target checks/unmounting, without a separate interactive UDisks unmount
or second authorization. Preserve helper-independent unmounted DD. Qualify the
bounded backend/client step before exposing mounted DD in Qt; include source/home
protection, identity changes, cancellation, readback, sync and worker/helper exit.

**CP17b7a completed (2026-09-26), native mounted DD operation.**
`WriteRawDevice` combines the existing retained caller-owned source, native
preparation target acquisition and unchanged raw copy/fsync/full-readback writer.
Raw eligibility is shared with `RawWritePlan`; inspection opens only a duplicate
of the retained source after ownership checks. Invalid/WIM/unaligned/oversized
sources fail before unmounting. `PrepareDeviceTarget::Table::Raw` permits claim
acquisition without applying filesystem/layout options; no formatter, mount,
installer, separate service or authorization request is introduced.

The caller owns confirmation, authenticated UID and the reporting/cancellation
context. This entry point is for an already authorized root job and initially
retains the preparation target's 512-byte-sector limit. It holds the exclusive
claim through writes, flush and readback, rechecks target identity/service state
before success, then closes source, target and retained caller home before return.
Its raw result retains phase, written/verified byte counts and flush failures.
It is not connected to the helper protocol or GUI yet; ordinary DD is unchanged.

The new `mbr.py --mounted-raw` simulated-USB recipe covers 14 native cases:
source changes/ownership, invalid marker/WIM/partial sector/size, changed disk
sequence, busy mounts, cancellation before work/after unmount/during copying,
flush failure, readback mismatch and success. Refusals preserve the whole disk;
copy cancellation still flushes; each raw progress callback checks exclusivity.
The final 64-MiB disk equals its source and passes independent filesystem/file
readback and BIOS boot. This validates the native integration, not a DD privilege
prompt. The test record gives normal/sanitizer results and retained evidence.

**Next: CP17b7b, one authorized DD helper/client job.**
Add an explicit raw operation to the existing one-request helper, with source
and target identity fields and no formatting/customization options. Reuse
`WriteRawDevice`; do not run a formatter or start a second privileged service.
Keep raw phases/byte counters distinct from ISO block progress (their operation
IDs overlap). Carry written/verified bytes, final phase and sync errors through
strict protocol validation, and join helper/launcher exit before completion.
Qualify cancellation/EOF, malformed or incomplete progress/results and a complete
mounted DD helper job before the separate Qt exposure. Preserve helper-independent
unmounted DD, including its current sector support and authorization route.

**CP17b7b completed (2026-09-26), mounted DD helper/client operation.**
The existing one-request helper accepts `raw-write-device-v1`, carrying only
source and target identities. It rejects formatting/customization fields and
calls the qualified `WriteRawDevice` operation. The same fixed helper path,
`--device` mode, pkexec launch and polkit action cover the whole job. File-mode
requests cannot select DD, and no secondary authorization service is added.

`raw-progress` uses native copy/sync/verify phases and byte counts, distinct from
ISO operation IDs. `raw-result` carries error, flush error, started/possibly-written
state, final raw phase and written/verified counts. The client checks phase order,
source-sized totals, monotonic counters, complete readback on success and clean
helper exit. Failure with unknown counters must preserve possible media changes;
it cannot claim an unchanged disk merely because no worker result arrived.
Closing the request pipe cancels cooperatively; cancellation/destruction waits
for helper exit. Existing ISO/format protocol frames retain their schema.

The ordinary-user protocol suite covers valid and malformed results/progress,
permission denial/dismissal, EOF cancellation/destruction, uncertain write state
and a result followed by delayed child exit. A simulated mounted FAT32 partition
passes four complete DD client/helper jobs: changed-source and busy refusals,
cancellation after native unmount without writes, and full 64-MiB DD copy/readback.
One launcher is counted per job; descriptors/children are released before
completion. Final whole-disk equality, independent filesystem readback and BIOS
boot qualify the output. See the testing record for final normal/sanitizer runs.

**Next: CP17b7c, mounted DD in Qt.**
Route eligible mounted DD selections through this helper, keeping unmounted DD
on its existing helper-independent UDisks path. Retain source identity and disk
sequence through confirmation, recheck eligibility before authorization, and fail
clearly if the mounted route cannot run. Map raw byte progress/results into the
current DD status/percentage model; do not use ISO block counters. Preserve the
existing ISO/DD selection dialog, destructive confirmation/default Cancel and
sync/verification/helper-exit-before-READY behavior. Qualify mode routing,
permission denial, cancellation and mount/identity changes in Widgets tests.
The new mounted DD backend remains limited to 512-byte target sectors; FUSE and
unresolved layered source/home backing remain separate work.


**CP17b7c completed (2026-09-26), mounted DD in Qt.**
The existing DD controller now routes eligible mounted targets to CP17b7b's
single authorized helper. Initial and pre-authorization UDisks snapshots accept
only eligible mount candidates; source identity and kernel disk sequence are
retained through confirmation. Local source ownership/path checks run before
launch. The helper repeats native checks, unmounts, acquires exclusively, writes,
flushes and reads back with the already qualified native operation.

An unmounted target retains the existing UDisks descriptor/worker route, including
when a mount disappears during confirmation. A mount appearing during confirmation
selects the helper before authorization. Once UDisks authorization starts, a new
mount fails the post-open check instead of triggering another prompt. A missing
helper fails clearly for mounted targets; it does not disable unmounted DD.

Qt maps raw byte progress into its existing copy/verification percentage model,
shows flush as busy, and reserves 100%/green READY and completion notification
until a complete verified result and helper exit. Cancellation and invalidation
wait for helper cleanup/exit. Unknown possible writes retain the incomplete-media
warning. ISOHybrid still defaults to recommended ISO mode; explicit DD uses the
same routing and destructive confirmation, with Cancel as the default.

Focused and full affected normal/sanitizer host suites cover mounted/unmounted
routing, mount changes on both sides of authorization, protected/changed targets
and sources, denial, sync failure, cancellation, service/device removal, raw
progress, delayed helper exit and completed-device removal. These use private
D-Bus/protocol fixtures; CP17b7a/b provide actual native/helper readback/boot
qualification. No new VM or Windows customization matrix is required for this
GUI wiring. See the testing record for counts and retained evidence.

**Next: CP17c, existing-multiple-partitions confirmation.**
Trace Windows's MSG094/MSG093 eligibility, ordering and cancellation behavior,
then use native partition discovery in the common Qt creation flow. Keep this
as a small GUI checkpoint with generated UDisks fixtures. FUSE/NTFS-3G mount
mapping, unresolved layered source/home backing and non-512-byte mounted DD
qualification remain explicit device work in the parity backlog.


**CP17c completed (2026-09-26), existing-multiple-partitions warning.**
The Qt flow now follows `src/rufus.c` MSG094/MSG093: after accepting the ordinary
erase confirmation, display an additional warning when the current target has
more than one counted partition. All current DD/ISO/non-bootable/SFD routes use
one dialog, default Cancel, and request authorization only after acceptance.
Cancellation leaves the device untouched; target removal dismisses the dialog.
The text retains the Windows wording with the host name changed to Linux.

The count follows `GetDrivePartitionData` in `src/drive.c`: MBR includes extended
containers but excludes unused entries, type 0xea and recognized type 0xef Rufus
FAT boot partitions. GPT excludes EFI System, Microsoft Reserved and partitions
named exactly UEFI:NTFS. Count only partition objects linked to the selected
table, not other disks or whole-device filesystems. Metadata does not alter the
device identity. GUI checks return a fresh count; stale discovery preview data
cannot suppress the warning. A newly multi-partitioned target after confirmation
fails before authorization, requiring START and review again; the raw descriptor
path also checks after acquisition.

Linux uses UDisks's FAT filesystem/label metadata to recognize the MBR Rufus
boot partition instead of reading FAT boot bytes as Windows does. Missing or
unrecognized metadata counts the partition and may produce an extra warning.
This conservative difference preserves warning-before-authorization ordering;
it neither adds a privilege request nor guesses that an unknown partition can
be ignored. Unknown partition types also count.

Generated metadata and Widgets tests cover exclusions, missing metadata, foreign
disks, warning order/defaults, successful acceptance, cancellation and removal
across all five current routes, and partition changes between confirmation and
authorization. Normal and sanitizer suites are recorded in the testing log;
this changes confirmation metadata/UI only and needs no new USB/VM/customization
qualification.

**Next: CP17d, fixed USB disk eligibility audit.**
Trace Windows's List USB Hard Drives conditions and defaults. Separate the
user's explicit opt-in from physical removable-media metadata without weakening
system/source checks. Audit discovery, retained selection, raw acquisition and
helper validation together, then qualify native paths before enabling the Qt
control. Remaining mounted FUSE/NTFS-3G, layered-backing and sector cases stay in
the parity backlog.


**CP17d1 completed (2026-09-26), fixed-USB audit and read-only recognition.**
Windows defaults `enable_HDDs` to false. The advanced checkbox and Alt+F toggle
it, refresh devices and print MSG253's status. The warning associated specifically
with this option is the MSG170 tooltip ("Enable the listing of USB Hard Drive
enclosures. USE AT YOUR OWN RISKS!!!"), not a separate HDD confirmation popup.
Creation still uses the ordinary erase and applicable multiple-partition warnings.
Keep this behavior when enabling the control; do not invent an extra modal prompt.

`src/dev.c` uses `IsHDD` (`src/smart.c`) to filter HDD/SSD candidates while the
option is off. That score combines Windows fixed-drive classification, capacity,
model and USB VID/PID tables. Card-reader size limits, virtual disks, non-USB
removable drives and Ctrl+Alt+F are separate branches. The opt-in does not disable
application/system-drive exclusions. Linux's current two removable flags are a
conservative native eligibility gate, not a port of this Windows score. Before
claiming full default-list parity, review reusing its pure scoring/data tables
with native facts rather than introducing another HDD heuristic.

A removable USB enclosure can have fixed media: Drive.Removable and
Drive.MediaRemovable are distinct. `DescribeUDisksDevice` now offers an explicit
read-only `allowFixedUsb` argument, default false. Only explicit USB transport,
known Boolean removable facts, available writable non-optical media, valid size
and path, and non-system/non-ignored whole disks are candidates. Returned facts
preserve both flags instead of relabelling fixed media as removable. Unknown
removable metadata is rejected. Existing production callers retain the default;
no fixed disk is newly listed or writable and the Qt control remains disabled.

**Integration findings:**

- `SelectionController`, `RawWritePlan`, all fresh UDisks checks and the helper
  currently reject fixed targets. Carry an explicit job-level opt-in through
  each layer; physical removable flags must never stand in for user consent.
- The helper's native source/home/mount inventory currently runs through
  `unmountTarget`, only when UDisks reports target mounts. Before allowing fixed
  disks, run/prove native system/source/home/holder checks even with an empty
  UDisks mount list. Qualify discrepancies and layered backing as refusals.
- Unmounted DD currently uses UDisks OpenDevice without the helper. Decide its
  fixed-disk acquisition path together with those native checks. Reuse the
  existing single helper job if required; never authorize twice or fall back
  after an authorization attempt.
- Keep opt-in disabled by default and frozen during a creation. Toggling it must
  refresh the list, discard any newly ineligible selection and avoid silently
  binding an old confirmation to another disk. Preserve default-Cancel warnings,
  raw/ISO choice, final sync/readback and helper-exit-before-READY semantics.

The read-only tests cover all four removable/media flag combinations, explicit
opt-in versus default behavior, and exclusion of internal, protected, read-only,
empty, optical, partition, malformed/unknown and invalid-path/size candidates.
They also verify that the production discovery/use checks still exclude fixed
media. Normal/sanitizer regression evidence is in the testing record.

**Next: CP17d2, explicit fixed-USB job policy and native acquisition.**
Carry the opt-in through the request and fresh checks while leaving the GUI
control disabled. Qualify fixed-media native/helper jobs on generated USB disks,
including empty/stale UDisks mount metadata, system/source/home/holder refusal,
cancellation, final sync and one authorization. Then CP17d3 can wire the checkbox,
Alt+F, original tooltip/status and refresh behavior with Widgets tests.


## CP17d2 — explicit fixed-USB helper policy and native acquisition

The helper request now carries an optional `allowFixedUsb: true`. Omission keeps
removable-only behavior, and the decoder rejects false/non-Boolean values and
file-image requests containing this device policy. Old requests are unchanged;
older helpers reject the additional field instead of silently ignoring it.
The policy is separate from the drive's actual removable/media-removable flags.
All helper snapshots and mount comparisons receive the explicit policy.

For opted-in jobs, acquisition checks the authenticated caller's home, retained
source, native system mounts, swap and device holders even when UDisks reports
no target mounts. The native and UDisks inventories must agree. After acquiring
the exclusive disk claim, repeat native protections and disk/source identity
checks before writing. Existing direct-filesystem/512-byte target limits remain;
unresolved layered source/home backing and FUSE target mapping remain unsupported.

Reuse the current helper for every opted-in DD job, including unmounted targets,
when the GUI integration is added. This keeps native protection, writing, sync,
readback and cleanup under one authorization. Ordinary unmounted removable DD
keeps its current UDisks path. No retry through a second authorization route.

The `--fixed-usb-helper` generated guest recipe exercises QEMU USB storage with
`removable=off`, explicit fixed-media metadata, seven native acquisition cases,
four mounted DD jobs and unmounted format/DD jobs. It checks protected and hidden
mount refusal, no-opt-in refusal, source changes, busy mounts, cancellation,
one helper launch per job, exit ordering, descriptor/child cleanup, full-disk
readback and boot. See the testing record for results and retained evidence.

**Next: CP17d3, GUI integration.** The checkbox remains disabled. Carry the policy
through discovery, selection, ISO preflight and RawWritePlan before enabling it;
freeze it for the complete job. Wire List USB Hard Drives, Alt+F and MSG170's
warning tooltip, with list refresh and stale-selection handling. Test original
erase/multiple-partition dialogs and READY ordering on each affected route.
Windows's default HDD scoring parity remains separate pending work.


## CP17d3 — List USB Hard Drives in Qt

Enable the advanced checkbox and Alt+F. Preserve the original MSG170 warning
and MSG253 + MSG250/251 temporary enabled/disabled message (3500 ms), with no
new HDD-only modal. The session starts with the option off. Keep unrelated
advanced controls disabled until implemented.

Selection owns the policy and refuses changes during analysis, mode selection,
preflight, confirmation, authorization and writing. Discovery refreshes when it
changes and rejects obsolete replies. Disabling it immediately removes fixed
candidates; if one was selected, leave selection empty instead of transferring
an old confirmation to another device. Keep the actual removable flags intact.
The existing conservative selection invalidation behavior remains intentional.

Freeze the policy in each ISO/format request and raw plan, including ISOHybrid
DD handoff. Fresh snapshots still enforce USB/whole-device, system/source,
read-only and identity checks. Every opted-in DD job uses the CP17d2 helper even
without mounts; the ordinary unmounted removable route stays unchanged when the
option is off. Missing helper, denial, source/disk changes and sync errors do not
fall back to another authorization route. Existing default-Cancel erase and
multiple-partition dialogs, ISO/DD choices and helper-exit-before-READY remain.

Qualification uses private-D-Bus and protocol-only Widgets fixtures, with
CP17d2's native generated-disk qualification retained as backend evidence. This
step does not access physical USB devices or repeat Windows customization tests.
See the testing record for full normal/sanitizer results and the expanded UI
screenshot. Existing direct-mount and 512-byte helper target limits still apply.

**Next: CP17d4, remaining default-list classification audit.** Compare Windows's
`IsHDD` scoring inputs with UDisks/sysfs facts and identify which shared rules can
be reused without importing Windows enumeration. Native removable flags are not
claimed as exact model/USB-ID/size scoring parity. FUSE target mapping, unresolved
layered source/home backing and other Windows device classes remain separate work.

## CP17d4 — HDD scoring audit and original-function reference tests

Completed the [classification audit](linux-hdd-classification.md). Rufus's
production scorer uses fixed status, binary capacity thresholds, friendly name,
VID and VID:PID tables; it does not use its test-only SMART commands. Native
UDisks removable flags do not reproduce its result. The audit maps Linux facts,
card exceptions, model-name differences and a small libudev adapter candidate.

Added a CMake reference target that compiles the original `IsHDD` verbatim with
fake platform facts and its unchanged tables. Thirty-two cases with debug off/on
pass normally and with ASan/UBSan/leak detection. Preserve the early table-scan
termination and wildcard indexing behavior when extracting; assess any upstream
fix separately. The harness covers ASCII strings, not Windows locale behavior.

No production Windows or Linux device policy changed. CP17d3 remains the current
GUI behavior. A key next-step constraint is explicit: a fixed-media flash drive
accepted by default scoring must still use the protected helper, even with List
USB Hard Drives off. Score acceptance is neither user opt-in nor proof that the
device is removable. Do not weaken native acquisition or add a second prompt.

**Next: CP17d5, shared scoring and native facts.** Extract one portable score
implementation and retain a thin Windows facts/logging wrapper. Qualify Linux
fixed/media, USB-parent IDs and model/card facts read-only; then requalify fresh
checks and helper routing before changing default eligibility. Keep missing facts
explicit and preserve all system/source/whole-device and read-only exclusions.

## CP17d5a — shared HDD score extraction

Completed the reusable part of CP17d5. `src/hdd_score.c` owns the original
arithmetic, matching order and quirks, using the unchanged `hdd_vs_ufd.h` tables.
The Windows `IsHDD` wrapper retains native fact acquisition and exact diagnostic
presentation. Build-selected string headers preserve the Windows APIs and use
native Linux equivalents without platform branches in the algorithm. Registered
in Linux CMake and the existing Windows Autotools/Visual Studio builds.

The frozen original function and current wrapper/shared scorer agree in 32
expected cases and 5,496 table combinations, with debug off/on and full diagnostic
comparison. Normal and ASan/UBSan/leak checks pass. Native Windows build/runtime
validation remains deferred; ASCII comparisons do not prove arbitrary Windows
locale behavior. Linux discovery does not consume the scorer yet.

**Next: CP17d5b, read-only native facts.** Implement and qualify a small adapter
for fixed-media status, USB-parent VID/PID, model and card facts, with unknowns
explicit. Then requalify fresh checks and protected helper routing before using
the scorer for default eligibility. Fixed-media flash accepted without checkbox
opt-in must still receive native system/source/home/holder protections through
the same single authorized helper. Keep score, user opt-in and physical media
properties separate.

## CP17d5b — read-only native disk snapshot

Completed the native snapshot portion: sysfs whole-device identity, expected
capacity, disk sequence, fixed/removable/unknown status, nearest USB-device
VID/PID and separate raw vendor/model strings. The existing direct sysfs approach
avoids a new libudev dependency. No device nodes are opened, and reads request no
privilege. Both ends of collection verify identity; changed or invalid snapshots
return no facts. Optional missing facts remain unknown.

Thirty Qt checks pass normally and with sanitizers, including coordinated changes
to disk sequence, size, device number and lookup path while attributes are read.
The full normal build and the shared-scorer reference tests also pass. No real USB
or large fixture was used, and default eligibility is unchanged.

**Next: CP17d5c, score-input and card policy.** Qualify a small mapping from the
native strings and UDisks card facts to Rufus's expected score inputs and card
exceptions. Preserve unknowns rather than inventing a Windows-friendly name.
Then requalify fresh snapshots and the protected helper route for default-visible
fixed flash before changing the default list. Native removable status, score
acceptance and checkbox opt-in remain separate facts. Real Windows validation
remains deferred.

## Production consolidation — completed 2026-09-27

Removed the parallel GUI DD worker/UDisks writer and use one existing helper for
all DD jobs. Consolidated writable-target inspection, partition/layout planning
and protocol phase tracking; separated read-only GUI selection from helper device
acquisition/mounting. Shared Windows algorithms and the existing native tools
remain intact. Production Linux implementation input lines fell from 14,644 to
14,332; no new backend framework was introduced. See the
[full record](linux-consolidation.md) for retained boundaries, the all-DD helper
and source-ownership requirements, exact counts and limitations.

Qualification passed: 39 normal suites, 24 sanitizer suites, 11 final-cleanup
rechecks in each build, native SFD/4K-acquisition checks, and mounted/unmounted DD
helper jobs with independent readback and BIOS boot. No physical USB or extra
Windows customization qualification. Guest artifacts were cleaned and VM targets
restored OFF. The next feature work remains CP17d5c, not a new consolidation phase.

## CP17d5c — native score inputs/card policy

Completed the read-only mapping in the existing native-facts adapter. Observed
vendor/model bytes and verified USB IDs feed the shared Rufus scorer; absent
fixed/removable facts stay Unknown. UDisks's explicit card identifiers select
the separate card branch. The unchanged 500-GiB limit is now shared with Windows
through `hdd_score.h`. Native names are not padded with invented Windows suffixes;
[the classification record](linux-hdd-classification.md) documents the resulting
limits to exact friendly-name parity and the original short-name quirk.

The full Linux build succeeds. All 66 native snapshot/policy checks and the
32 expected/5,496 differential scorer cases pass normally and with sanitizers.
Seventeen policy cases use real reads from temporary sysfs fixtures. No large
artifact, host device I/O, VM or extra Windows customization qualification.
The classifier is not yet connected to discovery or helper eligibility.

**Next: CP17d5d, fresh classification and protected acquisition integration.**
Use one classification result consistently in discovery, preflight and helper
revalidation. Keep physical fixed/removable state and List USB Hard Drives opt-in
separate from score acceptance. Requalify native source/home/system/holder checks
for default-visible fixed flash even when the checkbox is off and UDisks mounts
are empty, then enable the expanded default list. Retain one helper/prompt,
selection invalidation and exit-before-READY. Windows runtime validation remains
deferred; FUSE target mapping and other device classes remain separate work.


## CP17d5d — fresh classification and protected acquisition

Completed discovery, Qt selection and fresh GUI/helper checks through the same
native-facts reader and shared Rufus HDD/card filter. Linux no longer substitutes
UDisks removal flags for that heuristic. Fixed-media flash can appear by default;
large removable disks and cards above 500 GiB can require List USB Hard Drives.
Physical removal, filter acceptance and the checkbox remain separate. Native
classification facts are part of selection identity; changed or failed snapshots
cannot retain old consent. Missing disk sequence excludes a device, including
with opt-in, matching the existing helper's required identity contract.

Default-visible fixed/unknown media receives the existing native protections
before and after exclusive acquisition, even with no UDisks mounts and the
checkbox off. The root helper derives that requirement from fresh facts; no new
backend, scoring copy, authorization route or Windows change was introduced.
The [classification record](linux-hdd-classification.md) describes the exact
mapping and unresolved physical friendly-name parity.

Normal component/D-Bus/Widgets checks and the generated fixed-USB VM pass. The
VM covers 12 protection cases (default and opt-in), five Syslinux helper cases,
five default-policy DD cases and opted-in unmounted FAT32/DD. Independent file
readback, filesystem checks and BIOS boot pass. Authorization uses the existing
guest stand-in; physical drives/desktop polkit and Windows runtime remain
unqualified here. No new Windows customization qualification was performed.
The testing record carries sanitizer results and compact evidence. Disposable
VM images were removed and the regular build's VM option restored OFF.

**Next: CP17e, native target mount mapping for FUSE/NTFS-3G.** Inspect the existing
libmount/kernel inventory and native daemon/device facts; qualify a generated
NTFS-3G mount and refusal cases before enabling automatic unmount for it. Keep
source/home backing resolution explicit and fail closed when it is ambiguous.
Retain one helper, normal unmount only, exclusive acquisition and sync-before-
READY. Other filesystem, layout and Windows-media parity groups remain planned;
this is a bounded next step, not a new abstraction/consolidation phase.


## CP17e1 — NTFS-3G block mounts through the existing unmount path

The native mapping investigation found that ordinary NTFS-3G block mounts need
no separate mapping adapter. Linux's `fuseblk` branch uses the normal block
superblock path, retaining the actual block dev_t. See the pinned Linux 6.18
[`fuse_get_tree`](https://github.com/torvalds/linux/blob/v6.18/fs/fuse/inode.c#L1869)
and [block-superblock identity](https://github.com/torvalds/linux/blob/v6.18/fs/super.c#L1291).
The generated-device tests confirm that identity for both packaged `ntfs-3g` and
`lowntfs-3g`. Our existing private NTFS mount code already checks this contract.

The production change adds exact `fuseblk` to the existing accepted filesystem
list. It still requires the mount's kernel dev_t to be the target disk or a
native partition. A source name alone cannot authorize unmounting. Ordinary
anonymous `fuse`, unqualified subtypes and unresolved layered backing remain
outside this path. No PID scan, daemon-name trust, mount-source translation,
new public API or Windows change was needed.

Existing source/home/system checks, alias/nesting refusals, mount ID comparison,
normal unmount and exclusive acquisition apply unchanged. A pre-existing desktop
NTFS daemon is not adopted, killed or given another privilege request. The
foreground NTFS-3G process used when our helper creates its own private mount
continues to belong to that job, as before.

Both normal and ASan/UBSan/leak-detection guests pass all 63 native mount cases.
These include 18 real NTFS-3G cases (nine for each interface), a source-only
identity refusal, and the existing FAT/native protections. Normal unmount permits
immediate O_EXCL acquisition before waiting for the test daemon; it then exits
cleanly. Remount readback and independent `ntfsck -n` are clean. Three existing
helper/UDisks regression suites and the full normal build also pass. Guest data
is cleaned; compact evidence is in the testing record.

**Next: CP17e2, complete helper recreation from an NTFS-3G-mounted target.**
Exercise the real native inventory against the UDisks snapshot through the
existing helper, including a mounted DD or non-bootable creation, refusal and
cancellation, one authorization and exit-before-READY. This is integration
qualification of the same path, not a request for another mount backend.
Desktop polkit/automounter races, general anonymous FUSE backing, physical USB
and Windows runtime validation remain separate qualifications. Keep the existing
single Windows-customization test limit.

## CP17e2 — complete helper jobs from NTFS-3G-mounted targets

The existing MBR/helper fixture now starts complete jobs from real `ntfs-3g`
and `lowntfs-3g` mounts. It uses the production `PrepareImageProcess`, protocol,
helper, native inventory/unmount, UDisks matching and exclusive acquisition.
No production adapter or additional authorization route is introduced.

For each driver, check changed source, busy mount, caller home on target,
mismatched UDisks metadata and cancellation after unmount before writing. Then
create a non-bootable FAT32 volume and DD-write the complete generated bootable
image. Each job must use one launcher and wait for helper/launcher exit. Successful
results require sync and cleanup, while refusal/cancellation must preserve the
whole disk. Native daemon exit, descriptor/child cleanup, independent readback
and BIOS boot complete the qualification. The testing record carries results.

Normal and ASan/UBSan/leak-detection VMs pass all 14 new jobs and ten existing
helper jobs, including independent readback and BIOS boot. Existing helper-client
and Qt Widgets regression suites pass. Guest data is removed and both VM build
flags are restored OFF. Desktop polkit/automounter races and physical-device
validation remain separate qualifications.

The test observes mountinfo instead of repeatedly calling `stat()` on the active
FUSE mount: path observation must not itself keep the mount busy during normal
unmount. The desktop daemon remains test-owned and exits normally. The helper
does not adopt it, kill it, force unmount or bypass any protections.

**Next: CP18a, standalone UEFI:NTFS boot selection.** Trace Windows's advanced
boot option, NTFS/exFAT eligibility, MBR/GPT selection and native preparation;
reuse the existing resource, shared layout rules and formatter/helper paths.
Qualify the first complete source-free creation path before enabling its Qt
control, then cover remaining Windows-supported combinations in bounded steps.
Other device backings and desktop races remain in the backlog; they do not
require a new mount abstraction before moving to this separate parity feature.
Retain the existing single Windows-customization qualification limit.

## CP18a — standalone GPT/NTFS helper completion

Windows's `SetBootOptions` exposes UEFI:NTFS in the advanced boot list.
`SetPartitionSchemeAndTargetSystem` keeps MBR/GPT, disables BIOS, and prefers GPT
unless a prior partition selection applies. The shared `GetAllowedFileSystems`
limits this boot choice to NTFS/exFAT; `GetExtraPartitions` requests the bundled
UEFI:NTFS partition. `CreatePartition` writes the embedded image, and
`FormatThread` needs no ISO extraction or additional boot installation for this
choice. The result is a formatted data volume with the UEFI loader, ready for
the user to supply their boot files; it does not contain an operating system.

The existing Linux `prepare-ntfs-device-v1` request already supports source-free
GPT/NTFS preparation, including shared layout planning, native mkntfs and the
unchanged Rufus payload. Complete that route by calling the existing final disk
sync operation after payload copy/readback. The client now expects finalizing
after bootloader for source-free NTFS; an ISO request retains its existing
mount/extract/finalize stages. No new request type, backend, layout algorithm,
payload installer or Windows change is required.

Extend the existing small NTFS helper guest with final-sync failure, cancellation,
interrupted-sync retry and success. Each goes through the existing client and
one guest authorization launcher, checks the full payload, waits for helper and
launcher exit, and releases all device claims and descendants. Independently
inspect the final GPT, NTFS metadata/label/cluster size and exact bundled payload.
Keep the regular non-bootable NTFS regression cases in the same qualification.
The testing record holds the results and compact evidence; disposable media is
removed afterward.

Normal and ASan/UBSan/leak-detection guests pass all 16 helper jobs. Independent
GPT/NTFS/payload readback and five existing regression suites pass. Generated
media and guest files are removed; regular VM build flags are restored OFF.

This checkpoint covers the native **GPT/NTFS** source-free job. Qt selection,
MBR and exFAT standalone preparation remain unimplemented; do not present them
as supported or drop them from scope. No new firmware or Windows installation
qualification is implied by byte-for-byte payload readback.

**Next: CP18b, standalone GPT/exFAT preparation.** Reuse the existing native
formatter and the same boot-payload path. Distinguish an explicit bootable request
from the current source-free exFAT format operation, and consolidate shared
payload work rather than duplicating `prepareNtfs`. Qualify that path before
enabling the advanced Qt boot choice; MBR then needs the corresponding native
payload/layout support. Preserve Windows's eligible combinations and defaults.


## CP18b — standalone GPT/exFAT preparation

Add an explicit source-free UEFI:NTFS request for exFAT, carried by
`prepare-uefi-exfat-device-v1`. Existing `format-exfat-device-v1` requests keep
their format-only meaning, even when an older caller leaves `nonBootable` unset.
The explicit choice also accepts NTFS through its existing wire operation.
Reject source images, customization, non-bootable/DD/Syslinux options, file
fixtures and currently unsupported MBR/SFD combinations at the request boundary.

Use the existing native exfatprogs formatter and claim handoff unchanged. Share
payload materialization and installation with `prepareNtfs`; both use the same
embedded Rufus image, shared GPT placement rules, copy/readback writer and final
whole-device sync. One request predicate controls the reserved partition and
client bootloader phase. No new formatter, privileged process, boot resource or
Windows implementation is introduced. Qt's temporary payload must acquire its
filename before its source identity is captured: naming an anonymous inode can
change ctime, and the ordinary source-change checks must remain effective.

The existing small USB guest covers ordinary exFAT formatting followed by final
sync failure, cancellation, interrupted-sync retry and successful standalone
creation. Repeat the existing NTFS qualification to protect the shared path.
Check one authorization launcher per job, helper/launcher exit before completion,
full payload equality, descriptor/child cleanup and independent GPT/filesystem
readback. The testing record carries results. This does not add an operating
system, ISO-to-exFAT copying or a new firmware/customization qualification.

Normal and sanitizer guests pass 17 exFAT and 16 NTFS jobs each, with independent
filesystem/GPT/payload readback and no sanitizer or leak diagnostics. Six final
regression suites pass. Guest media is removed and both VM build flags return
to OFF; the testing record preserves compact evidence and reproduction steps.

The advanced Qt boot choice remains unavailable until the planned native
combinations are ready. **Next: CP18c, standalone MBR UEFI:NTFS preparation.**
Trace Windows's MBR types, flags and trailing-partition placement; extend the
existing native layout/payload code using shared Rufus rules. Qualify NTFS/exFAT
in bounded steps before exposing the advanced Qt selector with Windows's
eligible filesystems, GPT preference and UEFI target behavior. SFD remains
ineligible for this boot choice, matching Windows; other SFD work stays in scope.

## CP18c — standalone MBR UEFI:NTFS native layout and payload

Split the MBR work into native file qualification (this checkpoint) and complete
device/helper integration (CP18d), then expose the Qt control. Windows's
`CreatePartition` keeps the main NTFS/exFAT partition active with type `0x07`,
adds an inactive type `0xEF` UEFI:NTFS partition, and places that tail relative to
the full disk end. GPT instead subtracts its backup-table reservation first.
`WriteMBR` uses `write_zero_mbr` for forced UEFI, clearing boot code and the disk
signature while preserving the active main partition. Retain all these details.

Extend the existing native MBR planner/creator/checker with an optional payload
size, using the same shared Rufus placement functions as Windows and GPT. Keep
the existing 512-byte-sector, 2-TiB and filesystem geometry boundaries. Metadata
preparation clears both partition prefixes and the existing disk prefix/tail;
failures and cancellation flush attempted writes before returning. Validate both
entries, their exact active bytes, and unused entries. The new MBR payload entry
point shares GPT's retained-source, bounded-copy, sync/readback and final table
revalidation implementation. No new formatter, payload, authorization process,
layout framework or Windows change is needed.

Five new file-based suites qualify NTFS/exFAT placement, preparation and payload
writing, including malformed tables, clearing failures, short/interrupted I/O,
source/target changes, cancellation and sync/readback errors. Independent raw
MBR parsing and sfdisk check the table; full-file comparisons check preservation
outside the payload. The first fault run exposed an invalid inactive status byte
that libfdisk's boolean bootable query did not distinguish; raw readback now
requires the exact byte. All five pass normally and under ASan/UBSan/leak
detection. Existing partition and helper regressions are recorded separately in
the testing record. Generated media is removed after each suite.

This does not yet format either filesystem on the new layout or expose a bootable
MBR helper/GUI request. Old call sites explicitly request no trailing payload.
No firmware, physical USB or Windows customization qualification is added.

**Next: CP18d, complete standalone MBR helper jobs.** Route explicit NTFS/exFAT
requests through this layout and the shared payload writer. Reuse current native
formatters, retained disk claims, one authorization per job and final sync/exit
ordering. Qualify complete jobs on simulated USB before exposing the advanced
Qt selector, preserving Windows's filesystem eligibility, GPT preference and
UEFI target. Do not extend this request to ISO-backed MBR creation implicitly.

## CP18d — complete standalone MBR helper jobs

Add explicit `prepare-uefi-ntfs-mbr-device-v1` and
`prepare-uefi-exfat-mbr-device-v1` requests through the existing protocol. Only
source-free device jobs can select them; source images, customizations, SFD,
Syslinux and non-bootable flags remain incompatible. Existing GPT requests and
format-only MBR requests retain their original meaning. Carry CP18c's trailing
partition extent through shared format planning/preparation and dispatch to its
MBR entry point inside the same payload installation operation.

The existing NTFS formatter keeps the whole-disk claim while formatting the
main partition. exFAT keeps its existing bounded claim handoff, checks the saved
MBR on reacquisition and stops if that claim cannot be recovered. Both use the
existing filesystem readback, unchanged Rufus payload, final whole-disk sync and
helper/launcher exit ordering, with one authorization per creation. No alternate
formatter, partition algorithm, payload resource or Windows edit is added.

Extend the existing GPT standalone USB guest with `--uefi-mbr`, keeping the
ordinary MBR formatting regressions and independent partition/filesystem/payload
readback. The four standalone jobs cover final-sync failure, cancellation,
interrupted-sync retry and successful production-helper completion. Test fault
wrappers must exempt both GPT and MBR payload syncing so these faults occur at
finalization, after the actual payload copy/flush/readback. Repeat with
ASan/UBSan/leak detection and remove generated media after recording results.

Normal and sanitizer guests pass 17 NTFS and 19 exFAT jobs each, with independent
MBR/filesystem/payload readback and no sanitizer diagnostics. Eight existing
component/helper/payload regression suites pass. The test harness's final-sync
fault needed an MBR payload exemption matching its existing GPT exemption;
production code did not change after that finding. The testing record preserves
results and helper hashes. Disposable media is removed and VM build flags return
OFF. This remains helper-only until the next Qt checkpoint.

**Next: CP18e, advanced Qt UEFI:NTFS selection.** Expose the qualified standalone
GPT/MBR and NTFS/exFAT combinations with Windows's exact boot-choice eligibility,
GPT preference, UEFI target, filesystem/cluster defaults and no-source behavior.
Exercise selection changes and job/progress state in the existing Qt tests.
ISO-backed MBR preparation, other boot choices and remaining Windows parity
features stay in scope as separate checkpoints. Keep the single Windows
customization qualification limit.


## CP18e — standalone UEFI:NTFS in Qt

Completed 2026-09-28. Show UEFI:NTFS only with advanced drive properties,
following `SetBootOptions`. Hiding advanced choices restores BT_IMAGE without
losing its source. Store the original BT_* selection value instead of expanding
independent GUI boot-mode booleans. Selecting a new image returns to BT_IMAGE;
Win32 `EnableControls` also establishes that SELECT stays enabled in every idle
boot mode, correcting the previous non-bootable Qt restriction.

Use `GetAllowedFileSystems` and the existing native geometry checks to offer
NTFS/exFAT on GPT/MBR, with the embedded boot extent reserved. Preserve Rufus's
GPT preference, explicit partition choice, UEFI target, existing/manual filesystem
selection, default-marker ordering, shared cluster defaults and label behavior.
SFD and ISO-specific choices do not enter the standalone request. A loaded ISO,
its Syslinux state, DD choices, Windows customization and Alt+E must not affect
this source-free job.

Reuse the existing source-free preflight, confirmation, authorization and helper
lifecycle. Send the already-qualified CP18a–d requests. Use BT_UEFI_NTFS in the
shared progress calculation: the original discrete boot slots, no ISO analog
copy range and two finalization slots. A complete result must match the planned
layout, complete filesystem size and zero image-copy bytes. READY remains gated
on final sync/cleanup and helper exit. The same formatter, payload, native helper
and Windows source implementations remain in use.

Functional tests cover all four filesystem/layout combinations, retained image
transitions, normal and enlarged UI, frozen settings, one helper launch,
confirmation/recheck, missing tools, cancellation and failure during completion.
See the testing record for the normal and sanitizer evidence. This checkpoint
requires no additional large media or Windows customization qualification.

**Next: CP19a, FreeDOS reuse audit and backend boundary.** Follow the original
FreeDOS resource list, FAT16/FAT32 boot-record installation and locale setup in
`src/dos.c` and `src/format.c`. Identify code/resources that can be shared and the
small native file/attribute adapters required, then choose one simulated-media
backend checkpoint before enabling its Qt choice. Preserve the complete parity
backlog, the CMake-only Linux build and the single-customization test limit.


## CP19a — FreeDOS reuse audit and native compilation proof

Completed 2026-09-28. The [FreeDOS audit](linux-freedos-reuse.md) traces the
original ordinary-MBR/FAT16/FAT32/BIOS route and inventories all 27 resource
files (400,604 bytes), including the kernel's documented FORCELBA modification.
The unchanged ms-sys FAT16/FAT32 writers and physical-drive-ID functions compile
and execute on Linux through a small test-only stdio adapter. No production
source, Windows build or installed creation route is changed.

Normal and ASan/UBSan/leak runs pass four FAT16/FAT32 × label-preservation cases
and failures at all 16 original write positions. The probe checks BPB boundaries,
FSInfo allocation counters, boot signatures, the KERNEL.SYS reference and guard
bytes. It is a bounded-memory proof of reuse, not a formatted-media or BIOS boot
qualification. The full normal CMake/Ninja build also passes.

The native path must preserve the original Rufus MBR default (with Windows 7
bootstrap as its existing optional alternative), both FAT32 boot-record copies,
DOS file attributes and byte-exact startup files. Split the DOS-specific locale
tables and content from Windows host keyboard/code-page queries. Do not infer
a DOS keyboard from UTF-8 or copy the Windows FAT32 wrapper's unconditional
success after an inner failure; the shared writer itself reports that failure.

**Next: CP19b, a bounded FreeDOS FAT boot-record adapter.** Use the existing
native FAT16/FAT32 formatter on small regular-file fixtures, apply the original
writers and check primary/backup records, unchanged volume label/BPB/surrounding
sectors, extent guards, failure and cancellation. Keep the GUI/helper request
disabled until later payload/locale and complete simulated-USB qualification.
The audit records those later dependencies; do not combine all stages into one
unreviewable change. No new Windows customization tests are needed.

## CP19b — bounded native FreeDOS boot records

Completed 2026-09-28. `InstallFreeDosBootRecords` reuses the unchanged Rufus
ms-sys FAT16/FAT32 writers and physical-drive-ID routines through real, bounded
memory streams. Existing native FAT formatter validation owns descriptor,
extent and geometry checks. The adapter additionally checks the FAT32 reserved
coverage, FSInfo/backup locations and matching backup BPB/signature, staging
both records before the first target write.

The retained regular-file fd receives only sector 0 for FAT16 or sectors 0..2
and 6..8 for FAT32. The original `NO NAME` boot-sector label/BIOS drive-ID changes
remain intact; all other BPB bytes, FSInfo allocation counters, directory label
and surrounding bytes are preserved. Partial writes are flushed after errors
and cancellation. Success requires exact readback and the existing geometry
reader; no rollback or cache-bypassing persistence claim is made.

Normal and ASan/UBSan/leak runs pass 100 install/refusal/fault cases on small
formatted files, memory bounds/non-file rejection, the original 4,496-check
writer probe and 102 existing native FAT formatter cases. The full normal build
passes. GUI/helper binaries remain byte-identical to CP19a and original Windows
sources are untouched. All disposable media is removed; only compact evidence
is retained. No block-device acceptance, helper/GUI route or BIOS boot is added.

**Next: CP19c, shared FreeDOS payload and DOS locale content.** Follow the
[reuse boundary](linux-freedos-reuse.md): share the resource manifest and DOS
policy/generated bytes, retain Windows host queries in their wrapper, and
define the Linux host-fact adapter explicitly. Qualify native file copying and
FAT attributes before the complete CP19d helper/MBR/BIOS route and CP19e Qt choice.
Keep these dependencies in small reviewable steps; no additional Windows
customization qualification is required.

## CP19c1 — shared DOS locale policy and content

Completed 2026-09-28 as the first bounded part of CP19c. Moved DOS keyboard and
code-page tables, selection/fallback policy and startup-file content into
`src/dos_locale_policy.c/.h`. Windows `SetDOSLocale` uses these same functions;
its keyboard-detection and OEM-code-page query bodies remain byte-identical.
The shared writers use standard binary FILE streams with explicit CRLF and the
original OEM menu-border bytes. No Qt dependency or host query enters this module.

FreeDOS and MS-DOS preserve their existing driver order, fallback code pages,
US/437 no-menu case and content. The original FreeDOS EGA12 filename typo is
corrected to `ega12.cpx`, matching the payload. Missing friendly labels use the
canonical DOS ID rather than asserting/formatting NULL, and unknown code-page
names safely report `Unknown`. The Windows wrapper checks stream and close
errors rather than reporting false success; these focused corrections are
documented in the [reuse audit](linux-freedos-reuse.md).

Normal and ASan/UBSan/leak tests compare 468 output cases with a frozen original
oracle (four intended EGA12 filename corrections), plus explicit fallback/label
and I/O-error cases. The full Linux CMake/Ninja build passes. Windows Makefile
and MSVC manifests include the shared files; real Windows validation remains
deferred. No media or Windows customization test is required for this text-only
step. Linux GUI/helper routes are unchanged and temporary outputs are removed.

**Next: CP19c2, payload manifest and native file copying/FAT attributes.** Reuse
the 27 bundled FreeDOS files and their original destination/attribute/progress
rules. Keep native host keyboard/code-page discovery as a separately tested
adapter before complete helper integration; explicit DOS test inputs are not
host detection. CP19d MBR/helper/BIOS qualification and CP19e Qt enablement remain
dependent on completing these pieces. Keep the one-authorization/sync lifecycle.

## CP19c2 — shared FreeDOS manifest and native payload copying

Completed 2026-09-28. Windows extraction and Linux embedding/copying now consume
the same 27-file manifest, preserving resource order, root/LOCALE destinations,
hidden/system root-file attributes and five discrete copy milestones. The
original Windows resource compiler entries remain intact and are checked
against the manifest. Linux uses Qt's resource embedding through CMake; no
payload binaries are duplicated in the source tree or downloaded.

The native copy component borrows an empty directory on the existing private
kernel FAT mount. It creates files exclusively through retained descriptors,
sets/reads back hidden/system attributes on COMMAND.COM and KERNEL.SYS, and
checks every payload after file sync. Directory sync runs on failure/cancellation
too, retaining a separate sync error. Completed/partial output remains incomplete
on any error. The existing ISO empty-directory guard is moved unchanged into
`target_io` and reused rather than copied. This step publishes only payloads;
shared locale writers still need a native startup-file publication adapter.

All 31 copy fault/refusal/progress cases, 229 existing ISO/UDF extraction cases,
and DOS locale comparisons pass normally and with ASan/UBSan/leak detection.
A small Linux guest using kernel 6.18.52-1-cachyos-lts verifies the real FAT
attribute ioctls on FAT16 and FAT32. Independent mtools/fsck inspection after
unmount confirms all 27 payloads (400,604 bytes) and the expected attributes.
The 16/64 MiB volumes and guest kernel/initramfs are removed after preserving
logs/hashes. No physical USB, FreeDOS boot or Windows customization test is used.

**Next: CP19c3, Linux host facts and startup-file publication.** Define explicit
DOS keyboard/OEM-code-page selection from actual Linux host facts, with tested
fallbacks when facts are missing. Keep language, XKB layout and active keyboard
distinct. Write CONFIG.SYS/AUTOEXEC.BAT through CP19c1's shared writers under the
same retained-directory/error/sync rules. Then continue to CP19d's original MBR
selection, complete helper and BIOS qualification, followed by CP19e's Qt choice.

## CP19c3a — native FreeDOS startup-file publication

Completed 2026-09-28. `WriteFreeDosStartupFiles` uses CP19c1's shared DOS selection
and content writers, and CP19c2's existing exclusive file creation, sync and
readback. It writes CONFIG.SYS before AUTOEXEC.BAT and omits CONFIG for US/437,
preserving CRLF, OEM menu borders, driver selection and the original fallbacks.
It adds no content tables, progress slots, helper process or privilege request.

Both startup names are checked before writing, including an otherwise omitted
CONFIG. An existing name contradicts the freshly prepared target contract and
could preserve stale boot directives; Linux refuses it rather than truncating
or silently reusing it. Exclusive creation also refuses a file appearing after
preflight. Payload files remain untouched. Failed/cancelled publication keeps
partial output, syncs directory metadata and reports failure to its caller.
Successful publication is not successful READY: the existing full job still
owns final filesystem/device sync, unmount and helper exit.

The full Linux build and three focused suites pass normally and under
ASan/UBSan/leak detection: 64 startup cases, 31 payload-copy cases and CP19c1's
468 byte comparisons plus 26 explicit fallback/label cases. Startup tests include
13 frozen-original output comparisons, I/O/allocation/close/sync failures,
cancellation, existing-file/link refusals and two payload-plus-startup workflows.
They use disposable directories with the test FAT shim; no new disk or guest
image is needed. All temporary output is removed. Original Windows code and
shared DOS policy are unchanged in this checkpoint.

**Next: CP19c3b, Linux host facts.** CP19c3 was split after checking the host APIs:
Qt's input locale and systemd's configured keyboard are not proof of the active
desktop layout/variant. The [reuse audit](linux-freedos-reuse.md) records sources
and this distinction. Determine usable session facts before authorization, map
supported DOS equivalents and keep unknown/fallback reasons explicit. Current
tests supply DOS inputs; they do not qualify host detection. FreeDOS stays
disabled in the helper/GUI until host selection and CP19d's complete MBR/helper/
BIOS qualification are finished. No new Windows customization test is planned.

## CP19c3b — explicit FreeDOS settings instead of Linux host detection

Completed 2026-09-29. **User decision:** let the user select the FreeDOS keyboard
and code page rather than relying on Linux desktop settings. This supersedes
the host-detection work described in earlier checkpoints. Start at US/437 and
retain Rufus's existing startup content and boot-time US/437 alternative. The
Windows implementation keeps its automatic host selection unchanged.

`FreeDosLocaleDialog` provides two non-editable Qt Widgets selectors, populated
through small read-only enumeration functions over the shared DOS tables.
It lists actual FreeDOS driver IDs and EGA-supported code pages; no XKB/locale
mapping or second copy of the DOS tables is added. Final code pages are displayed,
so the original 850-to-858 upgrade is visible as 858. Unknown restored values
require an explicit correction. A keyboard change preserves the selected code
page; no host-derived or newly invented recommendation replaces the user's choice.

The intentional Linux UI difference has a concrete reason: desktop keyboard
configuration, active layout/variant and input language are different facts,
and Linux has no Windows OEM-code-page setting to copy. An explicit choice makes
the requested DOS settings clear and avoids depending on desktop-specific
inference. It does not add another privilege prompt. Present the chooser before
authorization/writes, treat Cancel as cancellation, and copy only accepted DOS
values into the immutable job request. Those main-window/helper connections
remain in CP19d/e; the tested dialog does not enable FreeDOS creation on its own.

The full Linux build passes. Normal and ASan/UBSan/leak runs cover the chooser,
shared catalogs, default independence from locale, invalid/restored choices,
Cancel, large fonts and generated Romanian/852 startup content, alongside all
64 startup-publication cases, 31 copy cases and the shared content regressions.
No host-detection implementation or DOSBox mapping data was imported. Test
directories were removed; no disk or guest image was created.

**Next: CP19d, complete FreeDOS preparation.** Add bounded explicit DOS choices
to the existing job request/helper, extend the original MBR bootstrap selection,
and join formatting, boot records, payloads and startup files in the existing
one-authorization/sync lifecycle. Qualify simulated media and a representative
BIOS boot, then connect the normal FreeDOS selector and this dialog in CP19e.

## CP19d1 — original Rufus/Windows 7 MBR selection

Completed 2026-09-29.

The existing native MBR writer now takes an explicit bootstrap enum instead of
a Syslinux boolean. It copies the original 440-byte `mbr_rufus.h` or
`mbr_win7.h` payload, as Windows's conventional BIOS branch does. Both retain
the per-job disk signature and use an active FAT16/FAT32 LBA partition. Existing
Syslinux, non-bootable and UEFI:NTFS callers select their previous code explicitly;
zero code still clears the first 446 bytes, including the disk signature.

The common planner rejects unknown bootstrap values, BIOS code with NTFS/exFAT,
and BIOS code combined with a UEFI:NTFS payload before any metadata clearing.
These are the currently qualified combinations, not a claim that the Windows 7
bootstrap itself is FAT-only. Readback checks the selected code as well as the
existing signature, active flag, partition layout and unused-byte invariants.
The existing clearing, cancellation, retained-descriptor and sync paths are reused.
No Windows source/build changes or new host detection are needed.

The full build, 37 selected normal suites (including a separate private-D-Bus
rerun and window/scaling tests) and 12 ASan/UBSan/leak suites pass. The new MBR
matrix covers 444 calls per run on disposable regular files; existing guest MBR
programs compile but were not executed. Two optional GUI fixture cases are
skipped. Temporary media are removed; details and limits are in the testing record.

**Next: CP19d2, helper assembly.** Freeze bounded DOS choices in the request and
join the existing formatting, FreeDOS boot records, payload and startup writers
inside one authorized job. Choose Rufus MBR by default and retain the original
Windows 7 fallback. Qualify complete simulated media and a representative BIOS
boot before CP19e exposes FreeDOS in the main GUI. This MBR component alone does
not enable FreeDOS, add an authorization step or establish firmware bootability.

## CP19d2a — explicit FreeDOS request and progress contract

Completed 2026-09-29. The full build, 13 selected normal suites and four
ASan/UBSan/leak suites pass; two optional real-ISO GUI cases are skipped.

The native request now reserves source-free FreeDOS FAT16/FAT32 operations with
mandatory MBR layout and explicit DOS keyboard, final code page and Rufus/Windows
7 MBR choice. Shared DOS policy validates the exact accepted values. Unknown,
fallback, noncanonical and embedded-NUL choices are rejected; the helper never
chooses host defaults. The chooser already normalizes 850 to 858, so the wire
contract requires 858. DOS settings are refused on other operations, and FreeDOS
cannot be mixed with source images, SFD, DD, NTFS/exFAT or Windows customization.
Native layout preflight selects CP19d1's existing MBR bootstrap enum.

The process freezes these choices before launching the existing single helper.
Its required phases are checking, partitioning, formatting, bootloader, mounting,
extracting and finalizing. A small `step` frame preserves the original negative
`UpdateProgress` events; the six-copy-step budget comes from shared Rufus progress
policy. Missing/extra steps, continuous copy counts, incomplete phases or missing
copied files cannot yield successful completion. Cleanup/sync errors remain fatal,
and success still waits for helper exit. No new job framework or privilege path
is introduced.

**Deliberate checkpoint boundary:** the real helper rejects FreeDOS with ENOTSUP
in its initial checking phase, before target acquisition or writes. This prevents
falling through the existing format-only path and reporting an incomplete stick
as successful. The guard is exercised through the real helper code with a test-only
UID shim, an ordinary child process and an unchanged disposable file; it acquires
no real privileges. The request/process tests simulate later helper responses,
not successful media creation. FreeDOS remains disabled in the main GUI.

**Next: CP19d2b.** Extend the qualified boot-record adapter to the retained native
device, then join formatting, boot records, the private FAT mount, bundled files
and selected startup content in this helper route. Remove the temporary rejection
only when that complete path is implemented and tested. Finish device/BIOS
qualification before CP19e enables the GUI choice.

## CP19d2b — complete FreeDOS device-helper assembly

Completed 2026-09-29. The helper now joins existing partitioning/formatting,
original MBR/PBR code, private FAT mounting, bundled-file copying and selected
startup content in one job. CP19d2a's temporary ENOTSUP guard is removed.

The PBR adapter uses the same staged ms-sys writers for regular files and retained
native block descriptors. Native validation reuses the formatter's identity,
capacity and FAT-geometry checks before and after writes. The helper also checks
the expected MBR and the target's retained identity. Planning and writing share
one MBR-choice function, including the original Windows 7 fallback.

The existing target owns the mount and its directory descriptor. Copying borrows
that descriptor and calls the already qualified resource/startup functions with
frozen DOS choices. Progress now traverses the real helper pipe: one preceding
copy slot and five bundled-file milestones. ISO and FreeDOS share finalization;
an earlier file-sync error is preserved across final filesystem cleanup. The
client still waits for helper exit before successful completion.

Qualification uses 64-MiB virtual USB disks, kernel FAT16/FAT32, native mkfs.fat,
the production helper and the actual process client. Each filesystem/MBR variant
checks stale-target refusal, mount failure, cancellation during the first copied
file, flush failure, unmount failure and success. Each job launches its helper
once; successful progress has exactly six copy steps. Independent host tools
verify MBR/PBR bytes, file inventory/content/attributes and startup content against
the original DOS writer oracle. No real USB or host mounts are used. The full
normal build, 18 selected normal suites and six ASan/UBSan/leak suites pass;
details and initial fixture corrections are recorded in the testing log.

**Next: CP19d2c, representative BIOS boot.** Create a small complete FreeDOS stick
through this helper and boot it under BIOS, verify the DOS prompt and a simple
command, then export evidence and remove the media. The block-device/media path
is qualified; firmware boot has not yet been exercised. After that, CP19e connects
the already implemented manual keyboard/code-page dialog and FreeDOS selector
to the normal Qt workflow, including Cancel, defaults, progress and READY parity.
FreeDOS remains disabled in the GUI until that qualification. Advanced BIOS
fixes, other geometries and remaining parity features stay in the backlog.

## CP19d2c — FreeDOS BIOS boot qualification

Completed 2026-09-29. Helper-created media now boots through BIOS to the bundled
FreeCOM prompt and executes VER/ECHO. Two representative paths pass: FAT32 with
the original Rufus MBR and explicit RO/852 startup, and FAT16 with the original
Windows 7 MBR and explicit US/437 startup. These are unchanged 64-MiB virtual
USB sticks from CP19d2b's real helper workflow and independent readback.

The second VM starts from SeaBIOS and the created USB; it receives no Linux
kernel/initrd, added DOS utility or substituted startup file. The observer checks
the requested DOS keyboard/code-page startup, the VER response followed by a
new prompt, and a separate ECHO result. Screenshots and text capture the evidence.
Snapshot mode leaves each verified backing image's hash unchanged. The recipe
reuses the existing QMP helpers and always removes the disposable media/guest
trees after saving compact logs. No production, GUI or Windows code changes
were needed for this checkpoint.

**Next: CP19e, normal Qt FreeDOS workflow.** Enable the selector with Windows's
MBR/BIOS/FAT16/FAT32 eligibility and defaults. Present the existing manual DOS
chooser before authorization/writes; Cancel aborts and accepted values stay
frozen. Connect discrete copy progress and the existing completion lifecycle,
then test retained-ISO transitions, labels, cancellation and READY behavior.
FreeDOS is still disabled in the main GUI until this wiring is finished. The
BIOS checks qualify representative SeaBIOS USB boots, not every physical
firmware or advanced BIOS/geometry option; those remain in the parity backlog.


## CP19e — normal Qt FreeDOS creation

Completed 2026-09-29. FreeDOS is enabled as a normal boot choice. The existing
selection/controller flow offers only MBR, BIOS (or UEFI-CSM), and native-qualified
FAT16/FAT32 settings, using shared Rufus filesystem/default/cluster policies.
The original Rufus MBR remains the default. Unsupported geometry disables START;
FreeDOS does not silently switch to GPT. Earlier image selection and applicable
manual preferences survive mode transitions without entering the FreeDOS request.

START opens the existing keyboard/code-page dialog at US/437, before checks,
erase confirmation, authorization or writes. Cancel aborts. Accepted choices
are validated by the existing strict request encoder and frozen with the device
and format selection. No Linux host detection, new privilege flow or separate
FreeDOS controller is added. The qualified helper handles the complete job.

The Qt controller now consumes its discrete copy events through the original
shared progress code: six steps and “Copying DOS files...”, including original
float rounding and the large-FAT32 weighting. Successful READY still requires
sync/cleanup and helper exit. The GUI tests explicitly hold the helper after
its result to verify that copying/result receipt cannot complete the job early.

The full build, focused simulated-device GUI cases, normal/scaled window and
GUI regressions, and selected ASan/UBSan/leak component suites pass. Test details
are in the checkpoint record. No additional native media or BIOS run is needed
for this wiring: CP19d2b/c established that backend and representative boot path.
No physical USB or new Windows customization qualification was used.

**Next: CP20a, standalone Syslinux reuse audit.** Trace Windows's advanced
Syslinux 4/6 choices against the already qualified native Syslinux 6 installer,
embedded resources, configuration and progress rules. Identify the smallest
shared/helper extension and the version-specific payload requirements before
enabling a new selector. Do not infer an unknown image version or add host
settings detection. Other BIOS/geometry, boot choices and parity work remain
tracked in the feature backlog; completing FreeDOS does not complete those.


## CP20a — standalone Syslinux reuse audit and blank-media probe

Completed 2026-09-29. The [source audit](linux-syslinux-standalone.md) maps
Windows's advanced Syslinux 4.07/6.04 choices to existing native components.
The current 6.04-pre1 executable already embeds the same Rufus loader/VBR and
its hash-pinned companion; a new downloader or bootloader implementation is
unnecessary for the first FAT route. Source-free installation must be explicit,
without synthesizing ISO metadata or weakening image compatibility checks.

A new regular-file functional probe formats empty MBR/FAT16 and FAT32 media,
installs with the unchanged native executable and independently checks output,
geometry and boot records. Both boot through SeaBIOS to the original bare
Syslinux prompt with no configuration/kernel supplied. Temporary media are
removed after saving compact evidence. The normal build and seven selected
suites pass; the GUI, helper and native-installer binary hashes are unchanged.
This is a test/source-audit checkpoint, not standalone GUI or helper enablement.

Two details must survive integration: Windows's standalone Syslinux progress
reserves three copy slots but emits two steps, and its v6 choice supports NTFS
as well as FAT. The native FAT provider cannot supply NTFS; upstream extlinux
has a candidate path requiring separate native qualification. Version 4.07 also
requires its own matching provider; no v6 substitution or silent removal from
scope is acceptable. Preserve native boot-file padding/attributes deliberately
as described in the audit; payload inputs and boot behavior are verified.

**Next: CP20b.** Add an explicit source-free v6 entry to the existing checked FAT
installer adapter, sharing target/process/readback/cleanup logic. Keep image
source/config checks intact and add focused adapter failure tests before helper
or GUI exposure. Follow with bounded standalone helper qualification, then the
advanced Qt selector. v4 and v6 NTFS remain separate, required parity work.


## CP20b — shared standalone Syslinux FAT adapter

Completed 2026-09-29. `NativeSyslinuxRequest` / `InstallNativeSyslinux` now accept
explicit standalone `BT_SYSLINUX_V6`, tied to the existing embedded 6.04-pre1
provider. All four ISO/source fields must be null for this selection; image jobs
remain the default and require their real retained source and analysis. Other
standalone versions, mixed requests and unsupported native geometry fail before
the installer starts. No inferred or synthetic ISO facts are introduced.

The standalone preflight requires a freshly formatted empty root, with an optional
volume label followed by the end marker. It rejects existing/deleted files,
directories and long-name records. Geometry and companion identity are reusable
checks; image source/config eligibility remains image-specific. The existing
target checks, child supervision, retained descriptor, exact Rufus loader/VBR
reconstruction, FAT-chain checks, sync and private-directory cleanup are shared.
No new provider implementation or generic registry is added. Production source
growth is 72 net lines including comments across the four adapter/payload files;
existing helper and guest-test callers only adopt the clearer API names.

Normal and ASan/UBSan/leak checks pass all 178 adapter cases: 46 standalone and
43 image cases for each of FAT16/FAT32. The nine selected normal suites also pass,
along with the source-layout/failure/UDF regression probe. Both whole-disk
regular-file probes now use the checked standalone adapter and boot as virtual
USBs to the bare Syslinux prompt. No configuration, kernel or OS is generated.
The pinned native executable's hash is unchanged. Disposable media are removed;
compact logs, source/binary hashes and boot screenshots are retained.

This checkpoint does not add a standalone helper request or GUI choice. The
existing device branch is shared, but source-free native block-device qualification
belongs to the next checkpoint. No physical USB, additional Windows customization
qualification, host-setting detection or Windows runtime test was performed.

**Next: CP20c.** Extend the existing bounded request/helper route with explicit
standalone v6 selection, MBR/FAT eligibility, phase checks and Windows's actual
two completed copy events (three allocated slots). Reuse partitioning/formatting
and one authorization; qualify success and failure cleanup on simulated block
devices before CP20d exposes the advanced Qt choice. Keep v4 and v6 NTFS as
separately qualified, required parity work.


## CP20c — standalone Syslinux helper and native-device qualification

Completed 2026-09-29. The existing bounded request now carries an explicit
standalone Syslinux 6 selection, encoded as a versioned FAT16/FAT32 operation.
Image jobs keep their original operation and default `BT_IMAGE` selection;
absence of an ISO never changes an image job into a standalone job. Source,
Windows/DOS customization, incompatible layouts/filesystems and other Syslinux
versions are rejected for this route before authorization or writes.

The single helper reuses native target acquisition, the original Syslinux MBR,
FAT formatting and CP20b's checked installer. Source-free installation needs
no private filesystem mount or extraction. It keeps the disk claim during the
installer, verifies the payload, and performs a final whole-disk sync/check.
There is no new privileged service, installer implementation or host detection.
The helper/client changes add 42 net production lines, including comments.

Progress follows checking, partitioning, formatting, bootloader and finalizing.
The original Windows allocation remains three copy slots with two actual events;
both events are emitted after successful native installation/readback. The
client rejects missing/extra/out-of-phase events, keeps extraction byte counters
zero, and cannot report successful completion before final sync and helper exit.

Seven normal and seven ASan/UBSan/leak suites pass. Eleven real helper cases per
FAT variant pass on disposable virtual USBs, including refusal before writes,
cancellation, process/readback/sync/cleanup failures and success. Each uses one
guest authorization launch and checks claims, descriptors, descendants, phases
and delayed exit. Both completed disks pass independent readback and BIOS boot
to the bare Syslinux prompt. The existing ISO helper route also passes its five
native jobs, readback and configured boot handoff. Compact evidence is retained;
guest trees and disposable media are removed. This does not qualify a new desktop
authorization prompt or physical firmware.

**Next: CP20d.** Enable the qualified standalone v6 FAT choice under advanced
drive properties using Windows's eligibility, defaults, mode transitions, status
text, confirmation and progress allocation. Reuse the existing controller and
one-helper flow. The choice is not yet available in the normal GUI. Version 4.07
and v6 NTFS remain required, separately qualified provider work.


## CP20d — advanced standalone Syslinux 6 Qt selection

Completed 2026-09-29. Expanding advanced drive properties now exposes
**Syslinux 6.04**, using Windows's display name for the pinned 6.04-pre1 provider.
The choice is MBR/BIOS-only and offers the already qualified FAT16/FAT32 formats.
Original shared filesystem and cluster policies preserve valid discovered/manual
preferences, default markers, capacity limits and the user's unrelated partition
preference. Collapsing the advanced section restores image mode without forgetting
the selected ISO. SELECT also switches back to an image normally.

The existing controller creates the explicit source-free request, normalizes the
label, checks the device/geometry/tools before confirmation and again before
one authorization. Retained images and Windows/DOS customization cannot enter it.
The confirmation names Syslinux, MBR, filesystem, label and cluster size, with
Cancel as default. The native helper/installer is unchanged in this checkpoint.
Production growth is 31 net lines including comments across five existing files.

The status is `Installing Syslinux 6.04...`. Progress reuses Rufus's three reserved
copy slots and two actual copy events, including large FAT32 weighting. It finishes
with the existing final-sync/cleanup/helper-exit gate, one notification and full
green READY; errors and cancellation remain incomplete. Unplugging afterward keeps
READY without another notification. This creates a bare loader, not a Linux OS.

Focused GUI coverage includes FAT16/FAT32/large FAT32 success, missing/disappearing
tools, confirmation cancellation, unplugging, errors, cancellation during phases,
retained DD/Linux/Windows image transitions and unsupported capacities. Normal and
sanitizer regression evidence and inspected GUI screenshots are recorded in the
[testing record](testing-and-checkpoints.md). Tests use temporary files, private
D-Bus and a protocol helper; CP20c retains the native simulated-device and BIOS
qualification. No additional Windows customization or physical USB test is added.

**Next: CP20e.** Audit and build a matching native Syslinux 4.07 provider through
CMake, checking its embedded loader/VBR against Rufus before exposing it. Reuse
the existing adapter/helper where its contract fits. v6 NTFS remains required,
with upstream extlinux as the candidate to investigate rather than writing a new
installer. Other advanced boot choices remain in the parity backlog.


## CP20e — native Syslinux 4.07 provider build and qualification

Completed 2026-09-29. The official 4.07 archive was checked against its upstream
HTTPS SHA-256 list (PGP signature not authenticated). Its 36488-byte loader and
512-byte VBR match Rufus's bundled v4 resources exactly. All 2279 extracted
release files remain unchanged after building. The shared CMake recipe now builds
optional `rufus-syslinux4` from nine original C files and upstream `bin2c.pl`.
`RUFUS_SYSLINUX4_UPSTREAM` selects that source independently of the existing v6
source. There is no upstream Make/Autotools or BIOS-payload rebuild.

Each provider has its own pinned payloads, generated files, executable and
ordinary-permission install component. Changed v4 payloads fail configuration;
a v4-only build and a combined build both work. The v6 executable SHA-256 is
unchanged. Runtime GUI/helper/adapter source has not changed in this checkpoint.
The new provider does not become an application choice yet.

Existing regular-file test infrastructure supplies the process runner, formatter,
MBR, independent readback, packaging and BIOS capture. Twenty-one supervision
cases and three native runner cases pass normally and with an instrumented
runner (normal native executable). FAT16/FAT32 build-tree and staged installs
boot as snapshot virtual USBs to the expected bare 4.07 prompt. Only
`ldlinux.sys` is installed: 37888 bytes after sector padding and two ADV sectors,
with protected attributes; v4 needs no companion. MBR/BPB/backup/alignment gaps
are preserved and boot does not alter the backing image. 4096-byte sectors are
refused without writes. Six selected normal suites pass, including the v6
package/standalone regressions and continued v4 application-request refusal.

This qualifies the candidate provider/runner on x86-64 regular files. It does
not qualify the checked v4 adapter, native block-device lifecycle, helper or Qt
choice. Upstream retains typed unaligned accesses and exit-time allocations;
there is no claim of a sanitizer-clean upstream executable. Its non-x86
`syslxint.h` accessor branches also have invalid pointer arguments/missing word
assembly and need correction before non-x86 host support. CMake rejects that
target architecture explicitly, with a configure test for that refusal. These are explicit
provider limitations; no sanitizer suppression or false cross-platform claim
is added. Native v6 NTFS remains separate required work.

Compact evidence is retained under parent `.rufus-test-artifacts/cp20e-*`.
Disposable media, copied firmware, staging and temporary v4-only build trees
are removed. The 4.07 archive/source is retained as the build dependency needed
for the next checkpoint and corresponding source. No host installation, physical
USB, new privilege prompt or Windows customization qualification was performed.

**Next: CP20f.** Extend the existing checked standalone adapter for the explicit
4.07 selection: choose its exact payload pair and padded length, omit the v6
companion requirement, and reuse Rufus's patcher, FAT readback, supervision,
flush and cleanup. Test both providers and their failure cases before exposing
v4 in the helper, then qualify native devices and Qt in later small checkpoints.
Keep image-version inference/compatibility unchanged until separately qualified.


## CP20f — checked standalone Syslinux 4.07 adapter

Completed 2026-09-29. `InstallNativeSyslinux` now accepts explicit
`BT_SYSLINUX_V4` with all four source fields null, on the same freshly formatted
FAT16/FAT32 contract as v6. Two small constant payload descriptions select the
original Rufus loader/VBR and padded length. Both embedded pairs are pinned at
CMake configuration; each has 192 extent slots, enough even for one sector per
extent. The original Rufus patcher reconstructs the complete expected loader,
ADV and VBR using the actual FAT chain. v4 needs no `ldlinux.c32`.

The FAT reader, target checks, retained descriptor, process runner, sync and
private-directory cleanup remain shared. There is no new installer algorithm,
provider framework or Windows source change. A mismatched native executable
fails exact readback after writes; callers must still supply the fixed trusted
provider for the selected version. It is not a pre-write executable verifier.
`BT_IMAGE` keeps the existing v6 compatibility rules; a v4 image report is still
rejected before writes. Helper/protocol/GUI support for v4 remains disabled.

Validation: full normal build; 274 normal and 274 ASan/UBSan/leak-checked adapter
cases across both FAT variants (46 standalone v4, 47 standalone v6 and 44 image
cases per FAT); packed-field, image-selection, helper-request and package/boot
regressions. The instrumented adapter uses normal upstream executables, not a
claim of sanitized upstream providers. Build-tree and staged v4 media boot as
snapshot virtual USBs to the expected bare 4.07 prompt after checked readback.
Independent inventory/FAT/MBR checks pass and backing-image hashes remain stable.
Optional-provider configuration is checked with v4 only, v6 only and neither.

Evidence is under parent `.rufus-test-artifacts/cp20f-*`; disposable disks,
firmware copies, staging and configure trees are removed. Upstream source/build
dependencies remain. No physical USB, host install, root prompt or Windows
customization test is involved. v4 non-x86 provider accessors and v6 NTFS remain
explicit work; neither is made eligible by this adapter change.

**Next: CP20g.** Extend the existing standalone helper route to explicit v4,
select its fixed trusted provider, and reuse the MBR/FAT preparation, original
progress allocation and complete-job sync/exit gate. Qualify simulated native
USB devices, failures and cancellation before exposing the Qt v4 choice in a
following checkpoint. Preserve image compatibility and one authorization.


## CP20g — standalone Syslinux 4.07 helper and native-device qualification

Completed 2026-09-29. The existing bounded helper protocol now accepts
`prepare-syslinux4-fat16-device-v1` and `prepare-syslinux4-fat32-device-v1`.
They explicitly select v4, require no image/source/customization fields and
reuse the v6 job sequence. Fixed installed paths select `rufus-syslinux4` for
v4 and `rufus-syslinux` for v6/image jobs; both preflight and installation use
the same mapping. Missing selected tools fail before writes. There is no
fallback to another version, request-supplied executable or new service.

One helper retains the device through original Rufus MBR preparation, native
FAT formatting, CP20f's checked installer and final disk sync/check. Both
versions reuse the three-slot Windows progress allocation and two actual copy
events after successful readback. The client requires finalization and helper
exit before success. The production change is 32 net C/C++ lines, including
comments; the original Windows and upstream implementations remain untouched.

Seven normal and seven ASan/UBSan/leak suites pass. The shared request test now
covers both versions (48 Qt results), including mixed/invalid requests, missing
or extra copy steps, sync/cleanup errors, cancellation and delayed exit. Its
first sanitizer run exposed a reused marker between the new version rows;
version-specific markers fixed that fixture race. The corrected test passed
normally and three consecutive sanitizer runs. No production workaround or
sanitizer suppression was added.

The existing simulated-USB guest passes eleven native jobs per FAT/version:
44 standalone jobs, four independent readbacks and bare SeaBIOS boots. The
five source-backed v6 ISO helper jobs and configured boot handoff also pass.
Guest tests check one authorization launch, retained exclusive claims inside
mtools, pre-write disk preservation, process/descriptor cleanup, two copy
steps and delayed helper exit. They use a guest-only authorization stand-in;
no new desktop password-dialog or physical-USB qualification is claimed.

Evidence is under parent `.rufus-test-artifacts/cp20g-*`. Disposable disks,
ISO trees, guest roots/initramfs and copied firmware are removed; the normal
build's `RUFUS_BUILD_VM_TEST` option is restored to OFF. No host installation,
new host-setting detection or Windows customization test was performed.
Image-version eligibility is unchanged; v4 remains absent from the Qt chooser.

**Next: CP20h.** Expose Syslinux 4.07 under advanced Qt drive properties,
matching the Windows label, MBR/BIOS/FAT eligibility, defaults, transitions,
status and shared progress. Extend the existing controller's standalone checks
and version-specific tool preflight; reuse the qualified helper flow and its
single authorization/completion gate. Test both versions and retained-image
transitions. v6 NTFS and non-x86 upstream v4 accessor corrections remain separate
required work; do not infer v4 ISO compatibility from this standalone route.


## CP20h — standalone Syslinux 4.07 in the advanced Qt chooser

Completed 2026-09-29. The advanced Boot selection lists **Syslinux 4.07** before
**Syslinux 6.04**, as Windows does. Both use the existing MBR/BIOS/FAT16/FAT32
controller/helper route, Rufus filesystem/cluster policy and native geometry
checks. Switching boot choices restores the default cluster size; collapsing
advanced properties restores the retained image. Windows/DOS/image settings
cannot leak into the standalone request.

Preflight checks the selected fixed installer before confirmation and again
before the single authorization. An absent other-version provider is irrelevant;
an absent selected provider stops preparation. Confirmation, installation and
completion status identify the chosen version. Both reuse the original three
reserved copy slots, two actual copy events and final sync/cleanup/helper-exit
gate before full READY and one notification. No new worker, adapter, installer
algorithm or Windows change is needed: production code grows by six net lines.

Ten affected normal and sanitizer suites pass, including the complete Qt workflow
suite. Tests cover both versions, small/large FAT choices, retained raw/Linux/
Windows images, ordering/defaults, provider checks, cancellation, failures,
progress and unplugging after READY. The first test expected a custom cluster
size to survive version switching; reviewing Windows's `IDC_BOOT_SELECTION` →
`SetFileSystemAndClusterSize` → `SetClusterSizes` confirmed the existing reset
was correct. Only that test expectation changed. v4 selection, installation and
READY screenshots were inspected. GUI jobs use a protocol fixture; CP20g remains
the native-device/boot qualification and was not repeated.

Compact evidence is under parent `.rufus-test-artifacts/cp20h-*`. Temporary Qt
fixtures are removed; no large media, host installation, physical USB or further
Windows customization qualification is involved. Image-version eligibility and
the v4 provider's non-x86 limitation are unchanged.

**Next: CP20i.** Audit and probe upstream v6 `extlinux` for Windows's standalone
NTFS choice before adding another installer. Establish its device/sector mapping,
mounted-target and retained-claim requirements against our preferred kernel
NTFS driver and NTFS-3G fallback. Reuse existing native mount/formatter/job code;
keep production NTFS eligibility unchanged until installation/readback and the
single-authorization lifecycle are qualified. Non-x86 v4 accessor corrections
and image-version compatibility remain separate required work.


## CP20i — native extlinux NTFS feasibility

Completed 2026-09-29. [The NTFS reuse audit](linux-syslinux-ntfs.md) and a
standalone CMake probe establish unchanged upstream 6.04-pre1 extlinux as the
candidate provider. Both the preferred kernel `ntfs` driver and `lowntfs-3g`
install the pinned original payloads on disposable MBR/NTFS USBs, map/read back
all 136 loader sectors, retain mounted-device claims and remount successfully.
Independent file reads and both bare SeaBIOS boots pass. No new production code,
installed binary, helper operation or GUI filesystem choice is added.

Keep the existing lifecycle: mount owns the partition claim after releasing the
whole-disk exclusive descriptor; pass the retained partition descriptor to the
supervised installer; sync/unmount/reacquire and finish the same authorized job.
The provider uses native FIEMAP on kernel NTFS and FIBMAP on NTFS-3G. Exact checked
payload/VBR readback and failure/cancellation qualification remain necessary.

The probe found a separate ntfsprogs-plus checker restriction: it rejects the
original Rufus/Syslinux `EB 58 90` jump despite successful native remounts and
boots. Keep the original boot bytes and preferred formatter. The audit records
the rejection and the metadata-only disposable-copy normalization used to
isolate it; no final media is repaired or altered to pass that checker.

Both new targets are test-only, absent from installation and builds with tests
disabled. Existing native FAT/process regressions and production format/request
gates pass. These are normal native guest/provider probes, not a sanitizer-clean
upstream claim or production authorization qualification. Compact evidence is
under parent `.rufus-test-artifacts/cp20i-*`; all disposable disks, guest trees,
initramfs and copied firmware are removed. No physical USB or Windows
customization qualification was used.

**Next: CP20j.** Add a checked standalone-v6 NTFS adapter around the candidate,
sharing the existing runner, payload descriptions and original Rufus patcher.
Validate mounted target identity, mapped loader/ADV/VBR and preserved metadata;
qualify failures, cancellation and sync before helper/GUI exposure. Keep image
eligibility separate and one authorization for the complete creation job.

## CP20j — checked standalone Syslinux v6 NTFS adapter

Completed 2026-09-29. The [NTFS audit](linux-syslinux-ntfs.md#cp20j--checked-native-adapter)
records the checked operation around unchanged upstream extlinux. It validates
the borrowed mount/partition, disk sequence and formatted geometry, supervises
the provider with the existing runner, then verifies mapped raw loader/companion
bytes, reset ADV, original BPB and backup VBR. FAT and NTFS share the existing
pinned payload descriptions and original Rufus patcher through one verifier.
No NTFS parser, replacement installer, host setting detection or second
authorization path is introduced; Windows source/build remains untouched.

The verifier uses bounded Linux FIEMAP/FIBMAP reads. Unsupported FIEMAP falls
back for NTFS-3G; I/O errors and invalid maps fail. Native kernel testing required
rounding the companion's final partial sector and invalidating clean raw-device
cached pages after filesystem/device sync, before comparing mapped data.
Started failures and cancellation still complete both flushes. The caller must
then unmount/reacquire/finish the same job before READY.

The normal and ASan/UBSan adapter each pass 48 cases on kernel NTFS and 48 on
NTFS-3G. Both final disks pass independent readback, read-only remount and bare
BIOS boot in each build. Six FAT installation regression suites pass in both
builds; the full normal build and three production eligibility/packed-accessor
checks pass. The external upstream provider remains unsanitized. Compact
evidence is under parent `.rufus-test-artifacts/cp20j-qualified`, `cp20j-asan`
and the associated logs. Disposable media/guest trees are removed.

**Next: CP20k.** Qualify Windows hidden/system/read-only NTFS file attributes on
both drivers and add the native extlinux provider to the main CMake/package
build. Helper lifecycle/selection and GUI enablement follow as a separate
checkpoint. Standalone NTFS is still unavailable in the application; source-backed
NTFS ISO compatibility and native v4 non-x86 accessor work remain separate.


## CP20k — NTFS file attributes and native provider packaging

Completed 2026-09-29. The [NTFS audit](linux-syslinux-ntfs.md#cp20k--windows-file-attributes-and-packaged-provider)
records the common `system.ntfs_attrib` interface on kernel NTFS and NTFS-3G.
Reuse that native ABI to protect `ldlinux.sys` as read-only/hidden/system and
keep the standalone `ldlinux.c32` ordinary, matching `src/syslinux.c`. Preserve
unrelated flags; read back changes before sync and check again with final
payload verification. Attribute errors fail; started failures and cancellation
still flush. No driver fork or POSIX-mode approximation is introduced.

The main/shared CMake recipe now builds `rufus-extlinux` from eight unchanged
upstream C files and the already pinned/generated v6 payloads. The test-only
provider definition is removed; only the mapping observer remains test-only.
`SyslinuxRuntime` packages both v6 executables with their shared license/notice.
A build with tests disabled also stages the correct production-only contents.
No upstream Make/Autotools, Windows source change or new dependency download
is involved.

The staged provider passes 59 adapter cases on each NTFS driver, normally and
under ASan/UBSan/LSan. Native-endian and big-endian attributes agree before/after
remount; independent file-record readback and all four bare BIOS boots pass.
The archive bit is preserved despite differing driver defaults. Existing v4/v6
package checks and four FAT boots pass, as do production request/format gates
and the packed-accessor check. Full normal build passes. Compact evidence is
under parent `.rufus-test-artifacts/cp20k-*`; disposable media/guest/staging trees
are removed. No host installation or physical USB is used.

**Next: CP20l.** Integrate standalone v6/MBR/NTFS with the existing authorized
helper. Preflight the fixed provider before writes; reuse partition placement,
NTFS formatting, private mount ownership and the checked adapter. Keep one
helper through unmount, FUSE child cleanup, disk reacquisition/identity checks,
final sync and exit, including failure/cancellation. Preserve Rufus's original
copy/progress events. Qualify that complete native lifecycle before GUI
eligibility is enabled in the following checkpoint. Source-backed NTFS ISO
compatibility and v4 non-x86 accessors remain separate required work.


## CP20l — standalone Syslinux v6 NTFS helper

Completed 2026-09-29. The [NTFS audit](linux-syslinux-ntfs.md#cp20l--standalone-ntfs-helper-lifecycle)
records the explicit `prepare-syslinux6-ntfs-device-v1` operation. It has no
source/customization fields or caller-selected provider, uses the native
formatter's serial, and rejects v4/NTFS and source-backed NTFS Syslinux.
The shared planner creates an original Syslinux MBR with one active NTFS
partition, without UEFI:NTFS. Existing FAT, ordinary NTFS and UEFI layouts stay
separate and retain their regression coverage.

The helper preflights mkntfs, the fixed installed `rufus-extlinux` and native
mount support before writes. It reuses the existing NTFS formatter and private
mount owner, borrows the validated partition/directory for the checked adapter,
and finishes through the existing cleanup path. That path now also flushes the
reacquired whole disk, including failures and cancellation. There is still one
authorized helper through filesystem sync, unmount, FUSE reaping, disk checks,
final sync and process exit. No new authorization path or host-setting detection
is added, and Windows source/build is unchanged.

The client accepts the NTFS mount phase and original two copy events; success
requires finalization and helper exit. Normal and ASan/UBSan/LSan native guests
pass 16 kernel-NTFS and 15 NTFS-3G jobs per build, with one authorization launch
per job, injected failures/cancellation, claim/process cleanup, independent
readback and four bare BIOS boots. NTFS-3G is exercised with kernel NTFS omitted;
a kernel mount error fails rather than switching to the available fallback.
Eleven existing FAT Syslinux jobs, six FreeDOS jobs, layout/protocol/file-helper
checks and the full Widgets regression also pass. Temporary media/guest trees
are removed; compact evidence remains under parent `.rufus-test-artifacts/cp20l-*`.

**Next: CP20m.** Enable standalone v6/MBR/NTFS in the Qt filesystem selection.
Reuse Windows's filesystem/default/cluster choices and existing progress/status
mapping, preflight the selected fixed provider before authorization, and keep
READY after cleanup and helper exit. Cover selection changes, missing provider,
cancellation and failure in the existing widget tests. v4 remains FAT-only;
source-backed NTFS Syslinux and wider image-version compatibility remain
separate required work. No additional Windows customization test is planned.


## CP20m — standalone Syslinux v6 NTFS in Qt

Completed 2026-09-29. Advanced **Syslinux 6.04** now offers NTFS alongside
FAT16/FAT32, with MBR/BIOS and the shared Windows filesystem/default/cluster
policy. Syslinux 4.07 remains FAT-only. Existing image and filesystem preferences
survive transitions, and selecting another boot type restores its default
cluster size as Windows does.

Only the native eligibility filter and existing controller change in production.
Preflight checks the selected fixed installer plus formatter before authorization;
NTFS needs no FAT installer/mtools. The existing native NTFS settings validator
replaces the FAT-specific geometry check for this path. Mount preparation uses
“Installing Syslinux 6.04...” and the original NTFS progress weighting/two copy
events. The controller also rejects a short formatted extent before READY.
One helper still owns installation, sync, cleanup and exit.

Normal and ASan/UBSan/LSan focused Qt runs each pass 74 results, including the
five NTFS cluster choices, retained ISO, v6/NTFS → v4/FAT → v6 transitions,
missing/disappearing tools, cancellation, mount/sync/cleanup failure and READY
remaining complete after unplug. Format, request and normal/scaled window gates
pass in both builds. The full Widgets regression passes 563 results, zero failures,
and two optional real-ISO skips. Selection/installing/READY screenshots are inspected.
The native provider/driver qualification remains CP20l's; no new VM/customization
matrix or physical USB is used. Evidence: parent `.rufus-test-artifacts/cp20m-*`.

**Next: CP20n.** Establish the source-backed NTFS Syslinux reuse boundary before
opening its GUI eligibility. Compare Windows's install-before-extraction order,
root loader/companion replacement, config patching and version selection with
the existing FAT ISO path and checked NTFS adapter. Use one matching v6 fixture
to qualify the native install/extraction sequence and preserve its payloads.
Reuse the existing helper, extraction and config policy; do not build a second
installer or infer unknown versions prematurely. Wider image versions, v4
non-x86 accessor corrections and additional native cluster/sector qualification
remain required work. No additional Windows customization tests are planned.


## CP20n — Syslinux ISO/NTFS reuse boundary and native sequence

Completed 2026-09-29. The [NTFS audit](linux-syslinux-ntfs.md#cp20n--iso-extraction-and-native-ntfs-installation)
compares Windows's install-before-extraction order with Linux's already qualified
FAT order. NTFS can use the same extract-then-install sequence while keeping the
private mount/device claim. This preserves general extraction's empty-target
contract. Version selection, root loader skipping, config patches, backup and
redirect all reuse existing Rufus code. Root-companion replacement and config
lookup still require the existing conservative source checks before this can
be admitted as an image job.

One matching v6 fixture passes actual extraction, upstream native installation,
independent readback and configured BIOS handoff on kernel NTFS and NTFS-3G,
normally and with sanitized code. The probe exposed two NTFS-3G incompatibilities:
redundant ownership/mode changes and unsupported no-overwrite rename. The shared
Linux config/extraction code now skips already-matching metadata and falls back
to a no-overwrite hard-link/unlink backup only for unsupported rename operations.
Required metadata changes and other errors still fail; existing backups and
original content survive failures. No new installer, file-copy engine or
privilege path is introduced.

The full normal build passes. Normal and ASan/UBSan/LSan runs each pass 43 config
patch results and 243 ISO/UDF extraction cases. Both native driver sequences
and all four configured BIOS boots pass. Temporary disks, source fixture trees
and guest images, including failed attempts, are removed; compact evidence is
under parent `.rufus-test-artifacts/cp20n-*`. Qt/helper image eligibility remains
unchanged. No new Windows customization test was run.

**Next: CP20o.** Add an explicit image-source contract to the checked NTFS
adapter, sharing retained-source, version/config and companion checks with FAT.
Keep standalone empty-root validation, require the extracted image's root loader
to be absent, protect existing unrelated content, verify pinned payloads and
attributes, and recheck the source after installation. Qualify this bounded
adapter change before enabling source-backed NTFS in the existing helper and
then Qt. Do not infer unknown image versions from the user's filesystem choice.


## CP20o — Checked Syslinux image installation on NTFS

Completed 2026-09-29. `NativeSyslinuxNtfsRequest` now distinguishes standalone v6
from a retained-image request. It reuses the FAT adapter's source identity,
version/build, companion and config-route policy. The common geometry check
admits NTFS's 32-bit hidden-sector offset while retaining mtools' 32-bit byte
limit for FAT; the FAT adapter explicitly remains FAT-only. This does not select
an installer, infer an unknown version from a filesystem choice, or enable a
helper/Qt image request.

Standalone v6 still requires an empty root and no image fields. An image job
requires all four source fields, the checked extractor's output and an absent
root loader. An existing root companion must be a single-link ordinary file on
the target and match the pinned replacement. Source identity is checked before
source eligibility, immediately before the installer and after payload/attribute
verification. The existing mounted-device checks, raw-sector verification,
child supervision and error-path sync remain shared with standalone installs.

The same offline native guest now accepts `--adapter --iso`. All 166 image cases
and 118 standalone regression cases pass per build across kernel NTFS and
NTFS-3G, normally and under ASan/UBSan/LSan. Independent readback preserves the
extracted payload/config/backup tree; four configured-image and four standalone
BIOS boots pass. Source eligibility covers 50 ISO/UDF cases on NTFS in both
builds, with the FAT source matrix and normal/sanitized FAT adapter regressions
also passing. Existing format/preflight/protocol eligibility tests still pass.
Temporary disks, fixture ISOs and guest trees are removed; compact evidence is
under parent `.rufus-test-artifacts/cp20o-*`.

**Next: CP20p.** Wire the qualified image adapter into the existing NTFS helper
lifecycle: retain and validate the source before writes, extract while privately
mounted, then pass its analysis/view to the adapter. Share existing progress and
one-authorization cleanup, wait for sync/unmount/reclaimed-device checks and
helper exit before READY. Qualify that complete source-backed helper flow before
opening Qt eligibility. Other Syslinux versions, v4 non-x86 accessor corrections
and wider sector/cluster qualification remain separate required work.


## CP20p — Source-backed NTFS Syslinux helper

Completed 2026-09-29. The explicit `prepare-ntfs-syslinux-device-v1` operation
connects CP20o's image adapter to the existing device helper. The helper retains
and validates the source, applies shared Rufus image/version/config policy before
writes, creates/formats the MBR target, extracts into its private NTFS mount and
installs/verifies Syslinux there. One authorization covers the complete job.
The source is checked again after sync/unmount and device reacquisition.

The mounted copy body is shared with existing ISO jobs; no second extraction
engine, provider runner, privilege route or progress protocol is introduced.
Image jobs use the original block-based copying counters and no standalone copy
steps. Standalone v6 retains its two discrete copy events. Both modes reuse
error-path flush, FUSE supervision, unmount, device checks and helper-exit gating.
Qt image eligibility remains unchanged.

Kernel NTFS and absent-kernel NTFS-3G fallback pass 49 complete image jobs per
build (25 kernel, 24 fallback), normally and under ASan/UBSan/LSan. The 98 jobs
include source/config/payload refusals before writes, cancellation during copy
and installation, source changes after copying/installation, readback/attribute/
sync/cleanup errors, known and compatible-unknown successes. All four final
outputs pass independent readback and configured BIOS boot. Thirty-one standalone
NTFS jobs and five existing FAT image jobs also pass normally, with three more
boots. Normal component/protocol/Widgets checks and their selected sanitizer
regressions pass; temporary media and guest trees are removed.

**Next: CP20q.** Add the original UEFI:NTFS extra partition to source-backed
MBR/NTFS Syslinux jobs when Rufus's shared `GetExtraPartitions` requires it.
Reuse the existing MBR layout and embedded payload writer, then qualify BIOS
and UEFI handoff before opening Qt choices. CP20p deliberately rejects those
images before writes: accepting EFI files without their required boot partition
would violate Windows behavior. This is a remaining implementation dependency,
not an intentional Linux feature difference. Other Syslinux versions, wider
native geometry/host qualification and the remaining parity backlog stay planned.

## CP20q — Syslinux images with the original UEFI:NTFS partition

Completed 2026-09-29. NTFS Syslinux image requests can explicitly include the
UEFI:NTFS partition required by shared Rufus policy. Preflight recomputes that
policy from the retained image and refuses missing or unnecessary extra
partitions before writes. The existing x64 fallback-loader validator also
rejects malformed EFI applications.

The existing MBR layout, formatter and embedded payload writer now handle this
combination, retaining the Syslinux bootstrap, active main partition and random
disk signature. Installation still uses one authorized helper. No new production
partitioner, payload, extraction engine, privilege route or progress protocol
was added. BIOS-only images and standalone Syslinux retain one partition.

Normal and ASan/UBSan/LSan runs pass 30 kernel NTFS and 29 NTFS-3G jobs each,
including payload failure/flush/cancellation and both layout-mismatch refusals.
All four outputs pass independent readback and BIOS plus x64 UEFI boot. A further
25-job kernel NTFS BIOS-only regression also boots successfully: 143 helper jobs
and nine firmware boots in total. Normal component/protocol/Widgets and selected
sanitizer suites pass. Disposable media, guest trees and firmware copies have
been removed; compact evidence remains under parent `.rufus-test-artifacts/cp20q-*`.
The fixture establishes firmware handoff, not Secure Boot or an OS installation.

**Next: CP20r.** Expose the qualified NTFS image path in Qt. Derive the extra
partition from shared image policy, preserve Rufus's FAT preference and ISO/DD
choice, and freeze that layout with the job. Treat `syslinux && uefiNtfs` as an
ISO job throughout preflight, confirmation, progress and completion; existing
standalone-only checks must not misclassify it. Check image-load/default and
filesystem-switch behavior, cancellation and READY after cleanup/helper exit.
Other Syslinux versions, EFI architectures and wider geometry remain in the
parity backlog.


## CP20r — Qt integration of NTFS Syslinux images

Completed 2026-09-29. Qt now exposes the CP20p/q-qualified NTFS Syslinux image
route, deriving its optional UEFI:NTFS partition from Rufus's shared image policy.
The same reservation feeds filesystem/cluster choices and the frozen request.
FAT preference, manual filesystem/partition choices and the ISO-recommended/DD
choice follow the existing shared Windows rules. Images requiring NTFS can now
retain the qualified MBR/Syslinux route instead of losing it to FAT-only filtering.

The existing UEFI:NTFS flag can describe an image's extra partition, so standalone
classification now explicitly distinguishes that case throughout preflight,
confirmation, progress and completion. Original NTFS image weighting and copied
byte checks remain in use. Successful READY waits for cleanup and helper exit;
no eject/remount behavior was added. The existing formatter availability check
is shared by ISO jobs too, before confirmation and before authorization.

Normal, scaled and ASan/UBSan/LSan GUI tests cover these selections and lifecycle
boundaries with generated ISOs and simulated targets. Existing standalone and
FAT routes remain covered. Native writes/driver cleanup and BIOS/x64 UEFI boots
remain qualified by CP20p/q; this Qt checkpoint adds no native backend or new
progress protocol. See the testing record for exact results and limits.

**Next: CP21a.** Audit the Windows GRUB2 installation/payload rules and native
Linux equivalents. Identify reusable Rufus code/data and the smallest necessary
adapter before implementing or exposing a new option. Plan file-backed/simulated
media checks and a representative BIOS handoff, then helper lifecycle integration.
Keep other Syslinux versions, EFI architectures, Secure Boot, geometry and the
full remaining Windows feature set in the parity backlog.


## CP21a — GRUB2 reuse audit and bare-loader proof

Completed 2026-09-29. Rufus already carries a prepared GRUB2 2.14 MBR and core,
with sector-1/sector-2 placement, a fixed `(hd0,msdos1)/boot/grub` prefix and its
selected modules/patches. The Windows path writes those bytes directly. Prefer
reusing them through the existing Linux boot-record/helper infrastructure over
introducing a host `grub-install` or another GRUB source build. The detailed
[reuse audit](linux-grub2-reuse.md) records the exact boundaries and alternatives.

A standalone CMake probe compiles the original `write_grub2_mbr` with bounded
native stdio and writes the unchanged core to generated regular-file media.
Normal and ASan/UBSan/LSan runs boot FAT16 and FAT32 as emulated USB under SeaBIOS,
verify the actual prefix/root and read a known directory entry. Entire-file
comparison confirms preservation outside the original write ranges. Four
invalid payload/gap cases per filesystem are refused unchanged, including the
63-sector old-BIOS gap. These are bare-loader/reader proofs, not menu or OS boots.
Generated media is removed; compact evidence is retained.

The audit also records Windows's standard PBR writes, Syslinux precedence,
filesystem/partition eligibility, original progress behavior and versioned
core-cache/download/fallback policy. Those cannot be skipped merely because the
bare core boots. The production executable and Windows sources are unchanged.

**Next: CP21b.** Implement the bounded boot-record adapter on regular files,
reusing the existing MBR writer, original payloads and portable SBR placement
logic. Generalize existing FAT PBR staging for the original standard records.
Test partial I/O, cancellation, sync/readback failures and untouched ranges.
Then qualify complete standalone helper jobs before exposing the advanced Qt
option. NTFS/ext and image-specific payload/module selection remain explicit
follow-up checkpoints; no host-settings inference or extra privilege prompt.


## CP21b — bounded GRUB2 boot records on regular files

Completed 2026-09-29. The existing MBR writer selects Rufus's unchanged GRUB2
array. Its new core entry point accepts already-selected immutable bytes,
reuses target/layout validation and buffered transfers, checks the embedding
gap, flushes partial writes and verifies the complete payload and MBR afterward.
The original FreeDOS FAT staging is now `fat_boot`, with an explicit choice of
FreeDOS or standard records through the unchanged ms-sys writers. FAT32 still
stages and verifies both primary and backup records. The existing helper's
FreeDOS call selects its previous payloads explicitly.

Normal and sanitized tests cover MBR preparation, standard FAT records, bounds,
partial I/O, cancellation, sync/readback failures and descriptor preservation.
Normal and sanitized FAT16/FAT32 disk images also boot through these production
adapters and read the expected directory entry. Existing FreeDOS, Syslinux,
non-bootable, UEFI:NTFS, SFD, helper and shared policy regressions pass. Generated
media is removed; details are in the testing record. Windows code and payloads
are untouched; neither a new formatter nor another installer was introduced.

**Next: CP21c.** Integrate standalone GRUB2 into the existing complete authorized
helper job. Retain immutable embedded core bytes and the exclusive whole-disk
descriptor, validate the simulated block-device path, preserve the original
progress allocation, and require sync/cleanup/helper exit before READY. Standard
FAT device records and GRUB core device writes remain gated until that path is
qualified. Add the advanced Qt choice only after the helper checkpoint passes.
NTFS/ext and source-backed GRUB module/version selection remain separate work.


## CP21c — standalone GRUB2 helper lifecycle

Completed 2026-09-29. The standalone FAT16/FAT32 route now uses the existing
authorized helper for the whole operation: preflight, MBR preparation, native formatting, original GRUB2
core, original standard FAT records and final device sync. The only added
request selection is `grub2`; exact versioned operation names reject images,
caller-supplied payload paths, other filesystems and conflicting boot modes.
The helper loads its compiled Rufus core before writes and retains those bytes
until installation completes. No host GRUB package or module discovery is used.

Both boot-record adapters now accept the retained exclusive block descriptor.
The existing target object still owns device identity, disk sequence, use/mount
checks and the formatter handoff/reacquisition. GRUB2 never mounts the filesystem
or copies boot files, and emits no file-copy events. The shared Windows progress
allocation is retained. Successful completion requires boot-record flush and
readback, final whole-device sync, claim release and helper exit. Cancellation
waits for partial boot writes to flush and the helper to exit.

Qualification uses 64-MiB emulated USB disks, the real helper and native formatter,
with isolated fixture authorization/UDisks and guest-only injected failures.
See the testing record for results and limits. The Qt choice remains unavailable
until the next checkpoint; Windows source and payloads remain unchanged.

**Next: CP21d.** Expose the qualified standalone GRUB2 FAT16/FAT32 route in the
advanced Qt boot list using Rufus's version label, MBR/BIOS and filesystem rules.
Reuse existing confirmation, immutable job selection, progress and completion;
cover normal, failure, cancellation and helper-exit ordering with Widgets tests.
Do not offer unimplemented NTFS/ext or ISO-backed GRUB installation yet. Those
remain required follow-ups, with source-specific module/version compatibility
and unknown image versions handled as recorded in the GRUB2 audit.


## CP21d — advanced Qt standalone GRUB2 choice

Completed 2026-09-29. The advanced Qt Widgets boot list offers Rufus's
`Grub 2.14` selection. It keeps MBR/BIOS and exposes only the FAT16/FAT32
filesystems qualified in CP21b/c; native format availability and shared Rufus
filesystem policy still gate the choices. An existing image can remain selected
in the GUI but is excluded from the source-free GRUB2 request. Switching back
to Image restores the image choice.

The option uses the existing one-authorization helper path, confirmation,
immutable request, Rufus progress allocation, cancellation and READY rules.
The final bar and success notification occur only after final sync, cleanup and
helper exit. The label comes from Rufus's original version header. Normal and
sanitized Widgets tests cover selection, FAT widths, formatter refusal,
confirmation, failure/cancellation and helper-exit ordering. No Windows source,
payload, boot writer or privilege flow changed in this checkpoint.

**Next:** continue GRUB2 parity in separate bounded checkpoints, starting with
the original standalone NTFS/ext eligibility and native boot-record/formatter
qualification. Source-backed GRUB needs distinct image version/module/prefix
selection and representative boot tests before its Qt route is offered. Keep an
unknown image version unknown until a matching native installer and payload are
qualified. Other Syslinux variants and the remaining Windows features stay in
the parity backlog.


## CP21e — GRUB2 NTFS MBR and core layout on regular files

Completed 2026-09-29. Rufus's existing MBR writer now accepts the standalone
GRUB2 bootstrap with an active NTFS partition, using the same partition type
and original embedded core placement as Windows. The change reuses the bounded
MBR/core adapters qualified for FAT and preserves their descriptor, gap,
flush/readback and cancellation checks. No NTFS formatter, partition boot-record
writer or new device path was added here.

Disposable regular-file tests cover NTFS layout creation/preparation, metadata
readback, original MBR bytes, core placement, refusal/fault cases and unchanged
ranges. Normal and sanitized results are in the testing record. FAT and existing
NTFS MBR routes retain their regressions. The advanced Qt GRUB2 choice remains
FAT16/FAT32-only; protocol and helper gates still reject NTFS GRUB2 jobs.

**Next: CP21f.** Adapt Rufus's original `write_ntfs_br` for the native NTFS
partition boot record, with bounded file I/O, readback, failure and unchanged
range tests. Then qualify the complete NTFS helper job and a representative
bare BIOS boot before offering NTFS in Qt. Ext2/3/4 and source-backed GRUB
remain separate checkpoints.


## CP21f — original NTFS partition boot record on files

Completed 2026-09-29. The Linux adapter now stages Rufus's original
`write_ntfs_br` output in bounded memory, preserving the native formatter's
BPB. It writes and reads back the exact 4,136-byte primary boot record, flushes
partial writes and leaves the backup NTFS sector unchanged, as the Windows
`WritePBR` path does. Shared ms-sys in-memory callbacks were moved from the FAT
adapter so both original writers use the same bounded compatibility layer.
The NTFS formatter's existing geometry, primary/backup and MFT validation is
reused before installing the record; no second formatter or host GRUB package
was introduced.

Disposable native-formatted files pass exact whole-volume comparison,
NTFS metadata/label readback, refusal, I/O fault and cancellation tests.
Normal and sanitized FAT boot-record regressions pass after the callback move.
The advanced Qt choice remains FAT16/FAT32-only and the helper still rejects
NTFS GRUB2; this checkpoint does not claim a complete NTFS USB or BIOS boot.

**Next: CP21g.** Connect the qualified NTFS MBR/core/PBR and native formatter
through the existing one-authorization helper. Test simulated block-device
ownership, exact format/boot phases, failure/cancellation, final sync and
cleanup, then boot a representative output through BIOS. Offer NTFS in Qt only
after that complete route passes. Ext and source-backed GRUB remain separate.


## CP21g — complete standalone GRUB2 MBR/NTFS helper

Completed 2026-09-29. The source-free request protocol now has a distinct
`prepare-grub2-ntfs-device-v1` operation. It rejects images, host payloads,
UEFI:NTFS, customization and nonzero caller serials. The existing single
authorized helper retains its whole-device claim, writes the original GRUB2
MBR/core, formats the NTFS partition with the native formatter, and applies
Rufus's original 4,136-byte primary NTFS boot record to that verified child.
The backup boot sector remains native, matching Rufus. The helper checks the
disk signature and partition entries before and after the child write, then
flushes the whole disk before reporting completion.

The simulated USB guest passes one successful full job and eight core/PBR
write, sync, readback and cancellation failures. The client requires the same
checking/partitioning/formatting/bootloader/finalizing sequence, no copy
events, and one launcher that has exited before READY. Independent media
inspection confirms original MBR/core/PBR bytes, native NTFS label/metadata
readability and a bare BIOS boot to GRUB rescue. Normal and sanitizer results
are recorded in the checkpoint log.

**Next: CP21h.** Offer NTFS in the advanced Qt GRUB2 filesystem list, retain
the current FAT behavior and test selection, preflight, confirmation, progress,
cancellation and READY in Widgets. Do not infer host GRUB settings. Follow
with ext-family boot records and source-backed GRUB selection separately.


## CP21h — Qt standalone GRUB2/NTFS selection

Completed 2026-09-30. The native filesystem policy now offers NTFS for the
advanced GRUB2 MBR/BIOS choice, alongside the already-qualified FAT16/FAT32
paths. GPT, SFD, exFAT and Syslinux mixing remain unavailable. The Widgets
selection controller, label normalization, native `mkntfs` preflight, existing
confirmation, one-authorized-helper launch, cancellation and READY handling
are reused without a new workflow or host GRUB detection. An existing NTFS
volume can preselect NTFS, while the first available format remains the Rufus
default. The protocol fixture checks the distinct NTFS operation and rejects
sources and UEFI:NTFS.

Focused normal/sanitized Widgets tests cover NTFS selection, exact request and
confirmation, formatter refusal/disappearance, cancellation at formatting and
bootloader phases, sync failure and READY only after the helper exits. The
full normal Widgets regression suite passes, including FAT GRUB2 and other
creation modes. See the checkpoint record for counts and limitations.

**Next:** Audit Rufus's ext2/3/4 standalone GRUB2 path for direct native
filesystem and original boot-record reuse before any Qt exposure. Keep
image-backed GRUB and module/version/prefix selection as separate checkpoints.


## CP21i — GRUB2 ext audit and MBR/core file boundary

Completed 2026-09-30. Windows Rufus offers ext2/ext3, but not a real ext4
choice: the advanced filesystem list gives no cluster entry to ext4 and its
formatter falls back to ext3 if ext4 is requested. ext2/ext3 require at least
256 MiB and use the single `0x100` default sentinel for partition geometry;
the formatter chooses the actual block size separately. The original GRUB2
MBR and embedded core apply unchanged and Windows performs no ext PBR write.

The Linux MBR planner now accepts only this source-free GRUB2/ext2/ext3 layout,
with active type `0x83`, original core placement and the existing descriptor,
readback and gap checks. A disposable sparse-file test covers both ext tables
and original core bytes, plus size, cluster, bootstrap, ext4 and UEFI:NTFS
refusals. The protocol, helper and Qt still reject ext. No filesystem or BIOS
boot result is claimed at this boundary.

**Next: CP21j.** Qualify native e2fsprogs formatting for ext2 and ext3 on a
borrowed partition extent. Use the existing formatter supervision and flush
contract; verify superblock/feature/label results and failure/cancellation.
Then integrate the one-helper device route and a representative bare BIOS boot
before exposing ext2/ext3 in Qt. Keep image-backed GRUB separate.


## CP21j — native ext2/ext3 formatting on borrowed files

Completed 2026-09-30. The Linux-only adapter uses the existing one-child
formatter supervisor to run an absolute `mke2fs` on a retained,
partition-sized regular-file descriptor. It passes explicit ext2/ext3 feature,
block, inode, reserve and initialization choices based on Rufus's original
size profiles, so host `mke2fs.conf` defaults do not choose a different format.
Only ext3 receives a journal; ext4 remains unavailable. The native creator OS
field is Linux, a deliberate metadata difference from Windows Rufus's bundled
ext2fs writer. A successful child and sync are followed by direct superblock
readback; any mismatch keeps the job failed.

Native `dumpe2fs` and read-only `e2fsck` independently inspect disposable ext2
and ext3 files, including the 512-MiB profile boundary, Unicode label and
reformat. Preflight, missing tool, false-success, child failure, corruption,
read failure and sync faults are covered. The full Linux GUI/helper is not
connected to this adapter yet; no USB, authorization or boot claim is made.

**Next: CP21k.** Add the retained block-device formatter boundary and connect
it to the existing one-authorization helper. Keep the original GRUB2 MBR/core
writer and no ext PBR. Test simulated USB ownership, faults and final sync,
then a bare BIOS boot before exposing ext2/ext3 in Qt.

## CP21k1 — ext2/ext3 block formatter boundary

Completed 2026-09-30. The native formatter now accepts a borrowed, exact-size
512-byte-sector block partition as well as the previously qualified regular
file. `PrepareDeviceTarget::formatExt()` permits only a source-free GRUB2 MBR
ext2/ext3 request. It checks the planned partition extent, request label and
filesystem, saves the MBR, refreshes the kernel partition table and checks the
opened partition identity. This checkpoint established the guarded API, but
not a successful formatter run on a block partition.

The Linux GUI and helper protocol still reject ext2/ext3. The regular-file
formatter suite now also refuses a zero device identity and a regular file
passed to the block entry point. Normal and sanitizer tests pass, and the
neighboring GRUB2 MBR/core layout test passes. This does not yet establish
that `mke2fs` can format a block partition or that a completed stick boots;
those checks require the simulated USB guest.

**Next: CP21k2.** Connect explicit ext2/ext3 GRUB2 protocol and single-helper
format/core/final-sync flow; run ext2/ext3 jobs, fault cases, independent
filesystem readback and bare BIOS boot in the disposable QEMU USB guest. Only
then enable ext choices in the advanced Qt list.

## CP21k2 — complete ext2/ext3 GRUB2 helper on virtual USB

Completed 2026-09-30. Explicit source-free GRUB2/ext2 and GRUB2/ext3 protocol
operations now route through the existing single privileged helper. The helper
preflights the same Rufus MBR plan and embedded `core.img`, formats the native
ext partition, writes Rufus's original GRUB2 MBR/core, and uses the existing
final flush and READY ordering. There is no ext partition boot-record write.
The default ext cluster sentinel is `0x100`; ext4 stays rejected. Qt still
does not offer ext2/ext3 until its advanced filesystem mapping is checked.

The first QEMU USB run exposed a real formatter conflict: `mke2fs` rejects
the partition while the helper retains an exclusive whole-disk claim. Even
two force options cannot overcome its later exclusive-open failure. The final
implementation reuses the existing FAT/exFAT handoff: keep the verified
partition descriptor, release the disk claim during `mke2fs`, then reacquire
and validate the disk, saved MBR and device snapshot. This remains inside one
helper launch and asks for authorization only once.

Disposable 320-MiB QEMU USB disks passed a complete ext2 job and 13 ext3
cases. The ext3 matrix covers preflight refusal, missing/failed formatter,
formatter cancellation, changed MBR, competing reacquisition, final-sync
failure/cancellation, GRUB2 core write/corruption/cancellation and success.
Native `dumpe2fs` and read-only `e2fsck` verified both final volumes, and
both disks reached GRUB's rescue prompt in a bare BIOS boot. The neighboring
file formatter, MBR/core layout and protocol/client tests pass. No physical
USB or Windows build was used.

**Next: CP21l.** Map Rufus's advanced ext2/ext3 choices and default cluster
sentinel into Qt Widgets, preserving Win32 visibility and selection behavior.
Exercise the real GUI choice and one simulated complete ext job before
declaring the feature exposed.


## CP21l — advanced ext2/ext3 Qt selection

Completed 2026-09-30.

The Qt Widgets standalone GRUB2 choice now offers ext2/ext3 only when advanced
format options are expanded and the target reaches Rufus's 256-MiB minimum.
Collapsing the panel rebuilds the shared filesystem list, retaining the selected
format when still available and falling back when ext disappears. The previous
explicit ext selection returns when the panel is expanded again, as Win32
retains `selected_fs` during an internal fallback. GPT, SFD,
image-backed GRUB and ext4 remain unavailable for this route.

The ext cluster sentinel remains `0x100` internally, but the combo and
confirmation display the single choice **Default**, as Win32 does. Source-free
requests omit the retained image, use a native UUID, preserve case and punctuation
in ext labels, check `mke2fs` before authorization and after confirmation, and
use the existing one-helper progress/cleanup path. Success requires readback
of the complete formatted extent before READY.

Windows's `FormatExtFs` copies the label into a 16-byte volume field with
`static_strcpy`, reserving a terminator. Linux uses the same 15-byte effective
limit in the GUI and confirmation. One deliberate boundary correction preserves
valid UTF-8: when byte 15 would split a character, Linux truncates before that
character rather than copying a broken encoding. Native formatter APIs retain
the full 16-byte field capacity for explicitly validated callers.

The normal format-policy and complete Qt window/write regression suites passed.
One GUI-driven ext3 creation on a disposable 320-MiB QEMU USB passed original
MBR/core comparison, native filesystem readback and bare BIOS boot to GRUB
rescue. The GUI remained Finalizing while helper exit was held, then showed
full READY; the whole-disk claim was released and removal preserved completion.
No physical USB or Windows validation was used. Disposable build/guest artifacts
were cleaned after exporting compact evidence to parent `.rufus-test-artifacts/cp21l`.

**Next: CP21m.** Audit and extract the original source-backed GRUB2 core
selection rules into shared policy: image version/build, known vs unknown version,
cache/download decisions, prefix differences and Syslinux precedence. First
qualify that decision table without enabling another creation route; then wire
one compatible image/core pairing through the existing extraction/helper path.
Standalone GRUB2 and source-backed GRUB remain distinct.


## CP21m — shared image-specific GRUB2 policy

Completed 2026-09-30. The existing shared GRUB inspection and format-policy
files now expose the BIOS/MBR core-lookup predicate, full-name/dot/dash candidate
iteration and original secondary boot-record precedence. Windows consumes the
same helpers, with no Linux conditionals or new policy module. Its cache reads,
Yes/No/Cancel warning, signed downloader and embedded fallback remain in the
existing Windows backend. The exact native integration contract is recorded in
`linux-grub2-reuse.md`, including full-name cache directories, read-failure
fallback without re-prompting, and lookup before Syslinux precedence.

Candidate iteration no longer temporarily modifies the image report. Unknown
versions stay unknown; an embedded or downloaded fallback is not a compatibility
assertion. GPT/UEFI, standalone and image-backed flows remain distinct.

Five Linux CTests pass, including generated ISO detection-to-lookup tests,
144 GRUB inspection cases, boot-precedence/format policy, native format options
and Qt window behavior. Shared policy tests also pass ASan/UBSan/LSan, and the
GUI/helper build passes. No host USB, network download or Windows execution
was involved. Compact evidence is in parent `.rufus-test-artifacts/cp21m`;
disposable builds and generated ISO fixtures were cleaned.

**Next: CP21n.** Add a bounded, unprivileged reader for the full-version GRUB
core cache. Reuse the lookup gate and preserve detection separately from payload
identity. Test absent/empty/short-read/oversized/changed cache inputs before
freezing a selected core for the existing one-helper job. Keep signed-download
handling and a representative compatible image/core/module boot as subsequent
checkpoints; source-backed GRUB must not be advertised solely because the
standalone embedded loader boots.

## CP21n — unprivileged full-version GRUB2 cache reader

Completed 2026-09-30. One small reader in the existing source library uses the
shared lookup gate and opens only the full detected name's cache. It preserves
the missing/opened-unreadable distinction needed by Rufus's prompt/fallback
flow. The existing native identity checker detects changed inputs; cancellation
or change stops selection. Accepted bytes are bounded by the planned embedding
gap and retained in memory with a SHA-256 fingerprint. Detection is unchanged;
neither the cache name nor fingerprint certifies module compatibility.

Three focused CTests pass normally and under ASan/UBSan/LSan; the new cache suite
has 48 cases, including real mutations and multi-chunk read faults. Qt GUI and
helper targets build. No network, USB, privileged job or Windows execution was
used. Evidence is in parent `.rufus-test-artifacts/cp21n`; temporary builds and
files were cleaned. This API is not yet connected to image-backed creation.

**Next: CP21o.** Audit the original signed bootloader-download verification
format, embedded key and native-library reuse. Qualify the native verifier with
file-only valid/tampered/truncated/wrong-signature cases before implementing
network downloads. Retain Rufus's warning and full-name/prefix candidate order
when connecting selection later. The accepted payload must then be frozen into
the existing one-helper job, followed by one representative compatible
image/core/module/configuration boot. Do not infer compatibility from the
standalone loader, cache naming or a successful signature alone.

## CP21o — shared download key and native signature verification

Completed 2026-09-30. The original RSA download key is now in one shared header,
consumed by the retained Windows verifier and a small native OpenSSL 3.0+ C
adapter. Its bytes are unchanged; Windows keeps its CryptoAPI implementation
and build. Linux verifies the original raw RSA-2048/PKCS#1 v1.5 SHA-256 format
with explicit parameters and no custom cryptography or caller-selectable key.
Only a zero result accepts bytes; rejected bytes remain caller-owned and must
be discarded by the future downloader.

The 36-scenario offline signature suite passes, including an authentic Rufus
server payload/signature, independent OpenSSL CLI verification, mutations,
wrong-key signatures, invalid arguments, unavailable-provider/setup/allocation
failures and recovery in one process. Cache and shared GRUB regressions pass
normally and under ASan/UBSan/LSan. GUI/helper targets build. Windows metadata
was checked; actual Windows validation is still deferred. The audit and exact
format contract are in `linux-download-signatures.md`. Small evidence is in
parent `.rufus-test-artifacts/cp21o`; temporary builds and fixture/key files
were cleaned, retaining only the small checked-in offline fixture.

**Next: CP21p.** Add a bounded, unprivileged Qt Network fetcher for `core.img`
and its detached `.sig`, returning owned bytes only after native verification.
Qualify it against a controlled local server for status/read failures, limits,
signature rejection and cancellation. Keep Windows's full-name/prefix selection
rules and embedded fallback when connecting the warning/cache flow afterward.
Frozen helper transfer and one compatible image/core/module/configuration boot
remain necessary before enabling image-backed GRUB creation.


## CP21p — bounded asynchronous signed bootloader fetcher

Completed 2026-09-30. One unprivileged Qt Network adapter fetches a candidate
and its original URL's `.sig`, uses the shared Rufus key/native verifier, and
returns owned bytes only on verified completion. It retains Win32's HTTP 200,
declared-length, original signature URL and 3,500-ms inactivity contracts.
Caller/gap and fixed signature limits bound accumulated bodies. Cancellation,
transport or verification failure publishes no bytes. Payload 100% remains
separate from verified completion. The audit in `linux-download-signatures.md`
records the deliberate refusal of HTTPS-to-HTTP redirects to preserve transport
protection. No additional privilege request or host detection was introduced.

All 63 local TCP/TLS downloader cases and three nearby CTests pass normally and
under ASan/UBSan/LSan; Linux GUI/helper builds pass. The tests use the small
existing signed fixture, no public-server download, USB, helper job or Windows
execution. Evidence is in parent `.rufus-test-artifacts/cp21p`; disposable builds
were cleaned. The component is not yet wired into image-backed creation.

**Next: CP21q.** Install accepted verified cores atomically into the full detected
version's unprivileged cache, using native Qt file facilities and the existing
bounded reader. Keep the downloaded candidate prefix distinct from the full
cache name. Qualify write/commit failures, unchanged previous cache on failure,
owned bytes and cancellation before connecting the Win32 warning/prefix-choice
flow. Use no new storage framework. Frozen helper transfer and a representative
compatible image/core/module/configuration boot remain required before enabling
image-backed GRUB creation; signature validity alone does not establish that
compatibility.


## CP21q — atomic full-name GRUB2 cache installation

Completed 2026-09-30. The existing cache component now stores verified downloads
using QSaveFile, with direct writes disabled and no extra storage framework.
Reader/writer share version/path/gap validation. The full detected cache name
stays separate from a shortened server candidate. Bounded chunk writes and the
existing cancellation query preserve previous contents until atomic commit;
failed/canceled saves leave no partial final file. This improves the original
direct cache write for the concrete reason that a later run must not read a
truncated bootloader. The audit records the caller's signature-verification
precondition, commit/cancellation boundary and disposable-cache durability limit.

The new 45-case storage suite and four nearby CTests pass normally and under
ASan/UBSan/LSan, including authentic fixture readback, exact gap boundaries,
real filesystem failures and injected write/commit/cancellation faults. Linux
GUI/helper builds pass. No physical USB, helper job or Windows execution was
used. Evidence is in parent `.rufus-test-artifacts/cp21q`; disposable builds
were cleaned. Image-backed GRUB is not enabled yet.

**Next: CP21r.** Connect the existing bounded reader, signed downloader and atomic
writer in one unprivileged selection flow, using the shared lookup/prefix rules
and Win32 Yes/No/Cancel contract. Retain missing versus opened-unusable caches,
full-version cache naming, cancellation, candidate failure/fallback and unchanged
detection. Qualify those decisions and stale image selection before starting a
helper. Then freeze accepted bytes for the existing one-authorized-helper job
and qualify one compatible image/core/module/configuration boot. Unknown versions
remain unknown, and neither cache naming nor signatures establish compatibility.


## CP21r — connected cache/download core-selection flow

Completed 2026-09-30. One unprivileged QObject in the existing Qt analysis library
connects the bounded reader, signed downloader and atomic writer using shared
lookup/prefix policy. Missing full-name caches request one Yes/No/Cancel choice;
opened-unusable caches fall back without re-prompting. No chooses embedded bytes,
Cancel stops, and Yes tries signed candidates in the original order, preserving
full-name cache storage and separate detected/server/payload identities. Failed
candidates are not selected; exhaustion and ordinary installed-read failures
retain fallback. A final missing/invalid signature instead stops explicitly
before authorization, preserving Win32's fatal signature error without relying
on its global formatter state. The result owns bytes; Embedded is an explicit empty-byte
choice for the existing bundled resource, not a compatibility assertion.

Existing source identity checks stop changed ISO inputs while prompting or
downloading. Explicit selection invalidation cancels pending work. Installed
readback must equal the verified snapshot before selecting a download; this
concrete native safeguard avoids choosing substituted cache bytes. Completion
is safe for immediate reuse/deletion. No helper or authorization starts here.

All 53 selection cases and six neighboring CTests pass normally and under
ASan/UBSan/LSan; GUI/helper builds pass. Tests use loopback HTTP and small local
fixtures, no USB, external binary download or Windows execution. The flow and
its limits are recorded in `linux-grub2-reuse.md`. Small evidence is in parent
`.rufus-test-artifacts/cp21r`; disposable builds were cleaned. The actual main
window warning and image-backed creation route remain unwired/disabled.

**Next: CP21s.** Freeze the selected core through the existing prepare request
and helper preflight, reusing retained-input validation and `WriteGrub2CoreImage`
rather than adding another writer/authorization route. Qualify bounded payload
transfer, changed/substituted inputs and embedding checks before target writes.
Then connect the main-window warning and qualify one complete compatible
image/core/module/configuration boot before enabling the image-backed route.
Keep Syslinux/SBR precedence shared, and preserve unknown detection. Do not
advertise compatibility solely from signatures, cache names or standalone boot.


## CP21s — frozen core transfer and helper preflight

Completed 2026-09-30. The existing process/request/helper now transfers optional
selected cores through private temporary files and bounded path/size/hash
metadata. One shared cache/helper reader retains validated bytes before target
access; the existing writer and single authorization route are reused. Eight
focused CTests pass normally and under sanitizers, including six actual helper
preflight cases with target access blocked. GUI/helper builds pass; evidence
was exported and disposable builds cleaned. See `linux-grub2-reuse.md`.

**Next: CP21t.** Connect the existing core selector to the actual Win32-equivalent
main-window Yes/No/Cancel warning and START lifecycle, retaining accepted image/
target identity before authorization. Then finish image-backed GRUB extraction/
helper integration and qualify one compatible image/core/module/configuration
boot before enabling creation. Keep original SBR/Syslinux precedence shared;
unknown detection stays unknown. Signatures and standalone boot do not establish
image module compatibility.


## CP21t — GRUB warning and START integration

Completed 2026-10-01 after resuming the saved 2026-09-30 checkpoint. The controller
uses the existing core selector before confirmation. The Widgets warning follows
Rufus's original Yes/No/Cancel behavior, including Cancel as the default and escape
button (Win32 notification resource/handler checked). Original download URLs are
shared through `src/download_urls.h`; the Windows values and RUFUS_TEST behavior
remain unchanged. Linux uses a writable per-user files cache because an installed
application directory normally requires privileges; the warning shows that path.
No second authorization route is introduced.

Download progress uses the shared Rufus percentage policy. Payload 100% while
signature validation is pending is still Downloading, not READY. Selection changes,
unplugging and cancellation close the warning and prevent helper launch. All
18 new GUI scenarios pass normally and under ASan/UBSan/LSan. They cover warning
choices/keys, invalidation, signed download, cache reuse, missing/bad payloads,
held payload/signature cancellation and excluded versions/partition/write modes.
The supported mixed Syslinux/GRUB route retains original shared Syslinux/SBR
precedence and launches only one simulated helper after confirmation.

The full Widgets suite passes normally (647 passes, two optional real-ISO skips,
about 141 seconds) and under sanitizers (same results, about 263 seconds), without
sanitizer findings or suppressions. Five neighboring suites and the focused GRUB
Widgets entry pass in both builds. GUI/helper builds pass. The previous 180-second
whole-suite timeout was insufficient; its limit is now 600 seconds. Per-case
waits remain bounded and unchanged. `rufus_grub2_widgets` runs the 18 new scenarios
independently with a 30-second limit. The two skipped checks require explicitly
provided Windows/Alpine ISO paths; no new GRUB scenario was skipped.

Logs, CTest transcripts and source hashes are retained in parent
`.rufus-test-artifacts/cp21t`; disposable builds and private fixtures are cleaned.
No real USB, authorization, external binary download, Windows customization test
or complete image-backed GRUB boot is claimed. Real Windows validation is deferred.
Pure image-backed GRUB creation remains disabled. Accepted core bytes are retained
but deliberately not sent to a Syslinux job.

**Next: CP21u.** Extend the existing prepare request and image checks to represent
an image-backed GRUB job, keeping source-free GRUB behavior and strict rejection
of unrelated options. Reuse shared boot selection and existing image extraction,
core staging/preflight and writer. Qualify the request boundary and helper order
before connecting/enabling the route. Then prove one compatible image/core/module/
configuration boot. Unknown detection stays unknown; a signature or standalone
boot alone does not establish image module compatibility.


## CP21u — GRUB ISO request and read-only preflight

Completed 2026-10-01. The existing GRUB2 operation and source identity fields now
represent either a standalone job or a retained FAT16/FAT32/NTFS ISO. No parallel
request, downloader or writer was added. Optional frozen core metadata uses the
existing bounded field. Source-free classification now distinguishes a GRUB ISO
from a standalone boot-record job; malformed/incomplete sources, customization,
DOS/Syslinux flags, incompatible layouts and ext ISO requests remain rejected.
Existing source-free GRUB/ext jobs are unchanged.

Read-only GRUB preflight shares the Syslinux BIOS report checks, Rufus's filesystem
and extra-partition policy, retained source checks, tree/config validation and
capacity accounting. Shared `GetSecondaryBootType` enforces original Syslinux/SBR
precedence. BIOS GRUB images do not require an x64 fallback EFI application.
Unknown version detection remains empty. Preflight success establishes extraction
prerequisites, not core/module compatibility or bootability. NTFS images needing
the UEFI:NTFS extra partition and ext ISO extraction remain pending, not removed
from parity scope.

The launcher and actual helper explicitly reject this unfinished image-backed
write route. The helper gate precedes source/core/target access. Two added actual
helper probe cases demonstrate ENOTSUP with no target gate reached, including a
missing staged core and nonexistent source. GUI support remains disabled.

Nine focused CTests pass normally and under ASan/UBSan/LSan without suppressions:
135 ISO preflight cases, 83 GRUB request cases, 44 core/helper cases, 66 process,
45 FreeDOS request, 71 Syslinux request, 53 selector and 20 focused Widgets cases
(including init/cleanup), plus 631 shared policy checks. New cases include six
real small ISO/UDF GRUB trees across three filesystems, policy refusals, stale
sources, unknown detection, source/core round trips and blocked write entry.
GUI/helper builds pass. The shared SBR declaration's existing C-linkage guard was
corrected; the Widgets test no longer needs its manual include wrapper. Windows
runtime validation remains deferred; no Windows algorithm changed here.

Compact logs, transcripts and source hashes are retained in parent
`.rufus-test-artifacts/cp21u`; disposable builds and synthetic fixtures are removed.
No real USB, authorization, external download, Windows customization qualification
or image-backed GRUB boot was performed. Full Widgets regression was qualified in
CP21t; this checkpoint uses the focused GUI/request/policy regressions above.

**Next: CP21v.** Reuse the existing FAT/NTFS source mount/extraction and GRUB record
writer inside the single helper. Correct image-job phase/copy accounting, retain
source/core snapshots before writes, and test helper ordering, rejection and
cleanup with simulated targets. Keep GUI creation disabled until a compatible
image/core/module/configuration boot has passed. Do not infer compatibility from
a signature, cache path, shortened version or the standalone boot qualification.


## CP21v — GRUB ISO helper integration

Completed 2026-10-01. FAT16/FAT32/NTFS GRUB ISO jobs now reuse the existing
single-helper extraction, core snapshot, partition and boot-record paths.
The helper rereads shared image/boot policy before writes, checks retained source
identity again immediately before partitioning, installs boot records before
extraction and uses the existing one-time copy finalization. No additional
production target abstraction, extractor, compatibility layer or authorization
route was added. Standalone GRUB behavior remains unchanged.

The launcher now expects checking/partitioning/formatting/bootloader/mounting/
extracting/finalizing for these jobs. Successful completion requires copied
bytes, all phases, successful sync/cleanup and helper exit. Optional selected
cores still use private staging and are removed after exit or failure.

Nine focused Linux CTests plus shared format policy pass normally and under
ASan/UBSan/LSan, without suppressions. GUI/helper builds pass. The new helper
suite covers 49 simulated jobs across the three filesystems; the GRUB request
suite adds 36 image lifecycle cases. Tests exercise actual native file formatting,
Rufus partition/core/PBR writers and existing ISO extraction/config rewriting.
Their test-only target supplies ordinary directories instead of mounts, and
NTFS formatting/PBR uses a separate partition file. This establishes helper
ordering and error propagation, not complete bootable filesystem contents,
real kernel mounting, unplug durability or image/core/module compatibility.
See [the test record](testing-and-checkpoints.md#cp21v--grub-iso-helper-integration).

Compact evidence is retained in parent `.rufus-test-artifacts/cp21v`;
disposable builds and synthetic fixtures are removed. GUI GRUB ISO creation
remains disabled. NTFS images requiring UEFI:NTFS and ext ISO extraction remain
pending parity work. No physical USB, external download or additional Windows
customization qualification was used; Windows runtime validation is deferred.

**Next: CP21w.** Qualify one compatible GRUB ISO/core/module/configuration boot
with the helper's native target in a disposable guest. Use the existing boot
harness and a pinned compatible source/payload; do not infer module compatibility
from a signature, cache location, shortened version or standalone rescue prompt.
Retain compact boot logs/readback evidence, remove disposable media, then connect
and enable only the qualified GUI route in a separate checkpoint. Unknown image
versions remain unknown until a compatible payload and module set is established.


## CP21w — compatible GRUB ISO native creation and boot

Completed 2026-10-01. A small generated GRUB ISO now passes full production
client/helper creation on a disposable QEMU USB, independent filesystem readback
and BIOS boot into Linux userspace. The fixture combines the existing signed
Rufus 2.12 core with pinned Ubuntu `grub-pc-bin` 2.12-1ubuntu7 modules and the
installed matching Linux kernel/initramfs. It does not require installing GRUB or
adding a production dependency. The complete 304-file module set is pinned;
normal.mod version detection remains the original shared Rufus inspection.

The native test found and fixed a helper progress-routing bug: every GRUB request
had been marked standalone, suppressing ISO extraction counters. Only source-free
GRUB now uses that classification; image copying retains BT_IMAGE reporting.
The simulated FAT16/FAT32/NTFS helper regression now requires nonzero, monotonic
copy counters with a stable denominator and the final shared block total.

Normal and ASan/UBSan/LSan guest creation and BIOS boots pass, with no suppressions.
Independent readback checks all 308 files, original MBR/core/PBR bytes, the selected
label/config patch and the precise existing NO NAME BPB/root-label diagnostic.
GRUB loads normal.mod/configuration, then its linux/initrd commands start Linux;
userspace mounts the virtual USB read-only and verifies its fixture marker.
Snapshot boot preserves the backing media hash. The driver checks seven ordered
phases, one simulated authorization, final sync/cleanup and child exit, private
staging/mount cleanup, disk claim release, descriptors and child processes.
Two focused component suites pass in both builds (119 and 51 Qt passes).
See [the test record](testing-and-checkpoints.md#cp21w--compatible-grub-iso-native-creation-and-boot).

This qualifies the tested BIOS/MBR/FAT32 core/module combination, not arbitrary
images, UEFI, other architectures, NTFS/ext GRUB images, physical hardware or the
unprivileged GUI/polkit interaction. Guest-only authorization and UDisks fixtures
remain test code; production guards are unchanged. GUI GRUB ISO creation remains
disabled. Unknown versions remain unknown. NTFS requiring UEFI:NTFS and ext ISO
extraction remain pending parity work; Windows runtime validation is deferred.

Compact evidence is in parent `.rufus-test-artifacts/cp21w`. Temporary module
package/extraction, builds, copied source trees, ISOs and guest/media data are
removed after logs/hashes/readback manifests are saved. User media is preserved.

**Next: CP21x.** Connect the qualified BIOS/MBR/FAT32 GRUB ISO route to the existing
Qt START/controller path, using the existing core selector and frozen handoff.
Preserve Win32 core choices/fallback warnings, shared Syslinux precedence and
ISO-recommended/optional-DD behavior. Test normal user flow, copy percentages,
source/selection invalidation, cancellation, one authorization and successful
READY only after helper cleanup/exit. Keep remaining filesystem/firmware routes
explicitly pending, and do not generalize this one fixture's compatibility proof.


## CP21x — GRUB ISO Qt START, progress and READY

Completed 2026-10-01. The Qt Widgets START flow now selects the existing GRUB2
BIOS/MBR/FAT32 ISO helper route. Eligibility uses Rufus's shared secondary-boot
policy, preserving Syslinux precedence. BIOS-only images select MBR; eligible
hybrids retain the MBR default and ISO-recommended/optional-DD dialog. Images
with regular EFI files can still select GPT and use the existing EFI copy route.
The confirmation shows the actual image, MBR/FAT32 layout and target system.

The existing core selector still applies Win32 version lookup, cache/download,
Yes/No/Cancel and embedded fallback rules. Its retained bytes now pass through
the existing private staged-core handoff, only when GRUB wins boot selection.
Changing a cache file after preflight cannot replace that snapshot. Unknown image
versions remain unknown; downloading a shortened version is not ABI certification.
No new job/writer abstraction, GRUB installation or production dependency is added.

GRUB ISO jobs use BT_IMAGE progress allocation rather than standalone GRUB's
three discrete copy slots. The copy percentage and overall FAT32 bar match the
original progress policy (50.0% copy gives 35061/65535 in the tested small-FAT32
job). Successful READY remains full green only after the helper's successful
sync/cleanup result **and** process exit. Sync/cleanup/copy/layout errors and
cancellation do not become READY. Removing the completed target does not undo
success or repeat the completion notification.

Only FAT32 is offered for this GRUB ISO route. Windows can offer additional
filesystems, but enabling routes without native compatible-image qualification
would overstate the current port's evidence. This is a temporary implementation
limit, not a change to shared Rufus policy or feature scope. The controller also
rejects direct FAT16/NTFS GRUB ISO requests before authorization. Standalone GRUB
and existing Syslinux/EFI filesystem choices remain separate. Native cluster
selection now validates an image's selected MBR geometry instead of rejecting
MBR image layouts before checking them.

The full Widgets regression passes 679 cases in both normal and ASan/UBSan/LSan
builds, with two existing optional real-ISO skips and no sanitizer suppressions. Focused GUI cases cover selected-core byte identity, cache mutation,
download/fallback/cancel, unknown version preservation, ISO/DD selection, frozen
requests, copy percentage, held finalization/result/exit, errors, source mutation,
removal and one simulated authorization. Component/format/source/process/window
and scaled-window checks pass. See the
[test record](testing-and-checkpoints.md#cp21x--grub-iso-qt-start-progress-and-ready).

GUI lifecycle tests use private D-Bus metadata, a protocol-only child and regular
files. They do not write real media, exercise real polkit, repeat CP21w's native
boot or qualify arbitrary core/module combinations. CP21w supplies the native
BIOS boot evidence. No additional Windows customization qualification was run.
Compact evidence is retained in parent `.rufus-test-artifacts/cp21x`; disposable
builds and generated GUI fixture media are removed after evidence export.

**Next: CP21y.** Qualify the same compatible GRUB ISO/core/module path on native
MBR/FAT16 media, reusing the existing helper/boot harness and independent readback.
Then enable that GUI filesystem in a bounded checkpoint. NTFS/UEFI:NTFS and ext
GRUB ISO combinations remain explicit pending parity work; Windows runtime
validation is deferred.


## CP21y — native FAT16 GRUB ISO creation and boot

Completed 2026-10-01. The same compatible pinned GRUB 2.12 ISO/core/module fixture
now passes production helper creation, independent complete readback and BIOS
boot on MBR/FAT16. This adds no production code or formatter/bootloader provider.
The existing FAT16 format, original Rufus boot records, selected-core staging,
ISO extraction/config patch, sync and cleanup work unchanged.

The existing test recipe accepts `--fat-bits 16`. On the 512-MiB virtual USB the
guest selects 8-KiB clusters; host readback independently checks FAT16 BPB/type,
cluster count, partition/hidden-sector/volume identity, original MBR/core/PBR,
all 308 copied files and the selected-label GRUB configuration patch. GRUB loads
normal.mod/configuration and boots the copied kernel/initramfs into Linux, whose
userspace mounts the virtual USB read-only and verifies its fixture marker.
Snapshot boot leaves the backing disk hash unchanged.

Normal and ASan/UBSan/LSan FAT16 runs pass without suppressions. The existing
FAT32 native recipe also passes after harness parameterization. Both host
component suites pass normally and under sanitizers (119 request and 51 helper
Qt passes). Guest audits check seven ordered phases, complete shared copy
counters, one simulated authorization, finalization/helper exit, source/core
identity, staging/private-mount cleanup, disk-claim release, descriptors and
adopted child processes. See the
[test record](testing-and-checkpoints.md#cp21y--native-fat16-grub-iso-creation-and-boot).

This qualifies the tested BIOS/MBR/FAT16 combination with 512-byte sectors,
not arbitrary source/payload ABIs, UEFI boot or physical media. It does not
exercise real polkit or the Qt START flow. Unknown image versions remain unknown;
NTFS/UEFI:NTFS and ext GRUB ISO combinations remain pending parity work. FAT16
ISO creation stays disabled in the GUI until the separate integration checkpoint.
The already qualified standalone GRUB FAT16 GUI route is unaffected.

Compact logs/manifests/hashes are retained in parent `.rufus-test-artifacts/cp21y`.
Disposable builds, extracted module package and all copied ISO/guest/media data
are removed after evidence export; user media and retained dependencies remain.

**Next: CP21z.** Enable FAT16 for eligible GRUB ISO selections using the existing
native cluster/format policy and controller. Test its actual Windows-style
filesystem/default selection, confirmation, core transfer, copy progress,
cancellation/failure and READY-after-helper-exit behavior. Reuse the CP21x
Widgets fixtures, keeping NTFS/ext and unqualified firmware combinations pending.


## CP21z — FAT16 GRUB ISO Qt selection

Completed 2026-10-01. The qualified GRUB ISO Qt route now offers FAT16 alongside
FAT32, intersecting original Rufus filesystem rules with native MBR geometry.
The preliminary GRUB eligibility check considers both formats, so FAT16-only
small targets no longer lose their BIOS route. NTFS/ext remain gated before
authorization until their compatible-image combinations are qualified.

Small BIOS targets regain Rufus's first supported filesystem default. GUI tests
cover 8-MiB FAT16-only, 64-MiB and 512-MiB FAT16-default, and 4-GiB FAT32-only
selection, including exact native default cluster sizes. The actual filesystem
combo, confirmation and frozen helper request agree. A new rescan test exposed
a product mismatch: a manual FAT32 choice was lost when a pure GRUB image had
no filesystem preference. The existing format-selection fallback now preserves
that choice, as Win32 PopulateProperties does. Shared image preferences still
win for images that have them.

Both FAT formats use the existing selected-core snapshot and one-helper ISO
lifecycle. Their copy percentage uses BT_IMAGE allocation: 50.0% file copy gives
35061/65535 overall in the tested small-target jobs. Tests hold copy, finalization,
successful result and helper exit separately; READY and its notification occur
only after exit. Denied authorization, sync/cleanup/copy/layout/extraction errors,
cancellation, source change and target removal do not become READY. ISO/DD
choice and version/download/fallback rules remain covered. No new formatter,
installer, writer abstraction, compatibility layer or production dependency
is introduced.

CP21w/y provide the native FAT32/FAT16 creation, complete readback and BIOS boot
evidence. This checkpoint checks the Qt workflow using private D-Bus, simulated
metadata, regular files and a protocol-only child. It does not qualify new
firmware combinations, actual polkit or arbitrary core/module ABIs. Unknown
versions remain unknown. No additional Windows customization qualification runs.
The full Widgets suite passes 693 cases normally and under ASan/UBSan/LSan,
with two optional real-ISO skips and no suppressions. Relevant component/window
checks pass in both builds. Compact evidence is retained in parent
`.rufus-test-artifacts/cp21z`; disposable builds/media are removed after export.
See the [test record](testing-and-checkpoints.md#cp21z--fat16-grub-iso-qt-selection).

**Next: CP21aa.** Qualify the same compatible GRUB ISO/core/module fixture on
plain BIOS/MBR/NTFS using the existing native formatter, mount provider, helper
and boot/readback harness. Prefer the new kernel NTFS driver and retain the
NTFS-3G fallback. Keep this a native qualification checkpoint before GUI
enablement; UEFI:NTFS and ext ISO combinations remain separate follow-up work.
Export compact evidence and remove generated builds/media when it finishes.


## CP21aa — native GRUB ISO on BIOS/MBR/NTFS

Completed 2026-10-01. No production code changes. The existing compatible
GRUB ISO guest/readback fixture now accepts NTFS with either the preferred
new kernel driver or the NTFS-3G fallback. Formatting uses the configured
ntfsprogs-plus mkntfs; original Rufus MBR/core/PBR, retained-source extraction,
configuration patching, sync and cleanup work unchanged.

Both providers pass normal and ASan/UBSan/LSan creation on a 512-MiB virtual
USB with 512-byte sectors and 4096-byte clusters. The guest independently
confirms the chosen driver, seven ordered phases, shared complete copy
counters, one simulated authorization, held helper exit, exact selected core,
unchanged source and released claims/descriptors/private mounts/descendants.
Host readback checks original boot records, NTFS BPB/backup geometry, label and
clean volume metadata, exact ordinary-file inventory and all 308 file hashes
with only the expected GRUB label patch. A separate BIOS boot loads the copied
normal.mod/configuration and reaches Linux userspace reading the USB marker;
snapshot boot leaves the backing-media hash unchanged.

The NTFS creation guests use Linux 7.2.7. The fallback guest omits its NTFS
module, so fallback is selected for unavailable support rather than to conceal
a failed write/mount. Both boot fixtures use kernel NTFS read-only. The same
pinned GRUB 2.12 core and complete module set are retained. Their precise
compatibility remains the qualification limit; unknown versions stay unknown.
The default FAT32 recipe also passes after harness extension, and both existing
GRUB request/helper component suites pass normally and under sanitizers.
See the [test record](testing-and-checkpoints.md#cp21aa--native-grub-iso-on-biosmbrntfs).

This native qualification does not enable NTFS in Qt, exercise real polkit or
write physical media. No Windows customization qualification runs. EFI/UEFI:NTFS
and ext GRUB ISO combinations remain separate pending parity work. Compact
evidence is retained in parent `.rufus-test-artifacts/cp21aa`; builds, extracted
package, copied ISOs/initramfs trees and all disposable media are removed.

**Next: CP21ab.** Enable the qualified plain BIOS/MBR/NTFS GRUB ISO Qt route
using existing selection, format policy and controller. Test filesystem/default
selection, confirmation/core transfer, NTFS progress allocation, cancellation,
failure and READY-after-helper-exit. Keep EFI-capable GRUB ISO NTFS choices gated
until their required UEFI:NTFS combination is qualified; preserve existing EFI
FAT/GPT routes. Reuse Widgets fixtures without new job/writer abstractions.


## CP21ab — BIOS/MBR/NTFS GRUB ISO Qt integration

Completed 2026-10-01. Plain BIOS GRUB ISO selections now offer NTFS alongside
FAT16/FAT32 through the existing controller/helper lifecycle. Preliminary
eligibility considers native NTFS geometry; shared Rufus filesystem policy
can therefore select an NTFS-only image when FAT is unsuitable. A valid manual
NTFS choice survives a rescan, and the actual filesystem/cluster combos pass
their accepted values into confirmation and the frozen request. NTFS uses the
formatter's serial (request volumeId zero), not a generated FAT serial.

NTFS copy percentages retain BT_IMAGE allocation with twelve filesystem steps
and three finalization slots: 50.0% copied gives 37027/65535 overall, compared
with FAT's 35061. Selected core bytes remain the retained snapshot despite cache
mutation. Tests hold copy, finalization, successful result and child exit
separately; READY and its notification occur only after successful helper exit.
The controller also requires complete NTFS format bytes for GRUB ISO completion.
A short-format result is FAILED even when the child reports success.

EFI-capable GRUB images do not offer MBR/NTFS yet: Windows's shared partition
policy requires UEFI:NTFS, which remains a separate native qualification. MBR/FAT
and existing GPT/UEFI routes remain available. The direct controller path also
refuses the unqualified combination before authorization. Tests cover BIOS/EFI
with ordinary and simulated large-file reports, including the NTFS-only BIOS
default and the retained GPT/UEFI route for an EFI image needing NTFS. ISO/DD
choice retains Rufus's recommended ISO default and permits DD with either format.

No new formatter, mount provider, installer, job/writer abstraction or production
dependency is added. CP21aa supplies normal/sanitized native NTFS creation,
complete readback and BIOS boot evidence for the kernel and NTFS-3G providers.
This integration checkpoint uses private D-Bus, simulated metadata, regular
files and a protocol-only child; it does not repeat Windows customization,
physical USB, real polkit or new firmware qualification. Unknown versions stay
unknown. See the [test record](testing-and-checkpoints.md#cp21ab--biosmbrntfs-grub-iso-qt-integration).

The full Widgets suite passes 711 cases normally and under ASan/UBSan/LSan,
with two optional real-ISO skips and no suppressions. Relevant component/window
suites pass in both builds. The staging cleanup component test now has a private
TMPDIR under each CMake
build. Its inventory must not observe concurrent GUI jobs' temporary cores;
the cleanup assertion remains unchanged. Compact evidence is retained in parent
`.rufus-test-artifacts/cp21ab`; disposable builds and generated media are removed
after export.

**Next: CP21ac.** Connect the source-backed GRUB/NTFS combination with required
UEFI:NTFS to the existing request/helper/partition/boot-record paths. Reuse
Rufus's shared extra-partition policy and bundled UEFI:NTFS payload, preserving
standalone GRUB and other jobs. Check binding/eligibility and file-backed layout
as one checkpoint, then qualify native creation and both firmware handoffs in
CP21ad before lifting the Qt gate. Keep GUI integration and remaining ext ISO
work in bounded follow-up checkpoints.


## CP21ac — GRUB ISO NTFS with UEFI:NTFS backend integration

Completed 2026-10-01. The existing NTFS helper can now prepare a source-backed GRUB job
with Rufus's required UEFI:NTFS partition. The explicit
`prepare-grub2-ntfs-uefi-device-v1` operation binds MBR, NTFS and the retained ISO
source; it cannot describe a standalone job, FAT/ext combination or Windows
customization. A GRUB request carrying UEFI:NTFS retains its ISO rather than
being classified as standalone UEFI:NTFS. The plain BIOS and standalone GRUB
operations keep their previous encoding and layout.

The existing MBR planner admits the trailing payload for GRUB/NTFS, using the
same geometry and shared extra-partition policy as other image jobs. Preflight
rejects missing or unnecessary UEFI:NTFS partitions and reuses the x64 fallback
EFI application check. Vendor-only loaders, malformed EFI applications and
loader read errors/cancellation are rejected. No new image-policy copy or
loader parser is added.

One authorized helper formats the main NTFS partition, installs the retained
GRUB core and original NTFS PBR, writes the unchanged embedded UEFI:NTFS image
through the existing bounded raw writer, and extracts the retained ISO. Both
bootloaders belong to one bootloader progress stage; raw payload operation IDs
do not escape into ISO copy progress. Payload write or sync failure prevents
mount/copy and successful completion. Private-mount finalization and helper exit
continue to govern completion and cancellation.

Qualification in this checkpoint uses disposable regular files, real MBR/core/
UEFI:NTFS writers and extraction, with a simulated device/mount target. Readback
compares the complete payload and retained core, checks the inactive EFI partition
and nonoverlapping extents, and verifies copied files/configuration. This does
not prove native partition-node lifetime, NTFS filesystem boot or firmware
handoff for the combined route. The Qt EFI-capable GRUB/NTFS gate remains in
place. No new formatter, mount provider, production dependency or writer/job
abstraction is introduced. Existing native CP21aa and GUI CP21ab evidence is
retained; Windows customization is not requalified.

Normal and ASan/UBSan/LSan runs pass all twelve relevant component suites
and the full Widgets suite (711 cases, two optional real-ISO skips). Production
GUI/helper targets build in both configurations. Compact logs, configurations,
source/binary SHA-256 inventories and cleanup records are retained in parent
`.rufus-test-artifacts/cp21ac`; disposable builds/media are removed after export.
See [the test record](testing-and-checkpoints.md#cp21ac--grub-iso-ntfs-with-uefintfs-backend-integration).

**Next: CP21ad.** Extend the pinned compatible GRUB ISO recipe with the existing
x64 handoff EFI fixture and bundled UEFI:NTFS image. Qualify production native
MBR/NTFS creation, independent complete readback, BIOS configuration/kernel boot
and x64 UEFI:NTFS handoff with the preferred kernel driver and absent-kernel
NTFS-3G fallback. Reuse the VM infrastructure; retain compact evidence and remove
disposable media. Only after that checkpoint should Qt lift the EFI-capable
GRUB/NTFS gate and test its selection/confirmation/progress/completion behavior.


## CP21ad — native GRUB NTFS with BIOS and UEFI:NTFS handoffs

Completed 2026-10-01. The compatible pinned GRUB 2.12 ISO fixture now also
contains the existing x64 EFI handoff application at `/EFI/BOOT/BOOTX64.EFI`.
The existing production helper creates the combined MBR/NTFS + UEFI:NTFS layout
on a 512-MiB virtual USB. It uses the same original GRUB MBR/core/NTFS boot record,
native formatter/mount/extraction paths and bundled UEFI:NTFS image connected
in CP21ac. Production source hashes match CP21ac; no production fix, installer,
writer, dependency or abstraction was needed in this qualification checkpoint.

Four complete creation/readback/BIOS/x64 UEFI runs pass: the preferred kernel
NTFS driver and absent-kernel NTFS-3G fallback, normally and under ASan/UBSan/LSan.
The creation guest uses kernel 7.2.7-arch1-1 and ntfsprogs-plus 1.0.0-3. Fallback
guests deliberately omit the kernel NTFS module; the helper must log NTFS-3G and
reap its FUSE process before completion. One simulated authorization, retained
source/core snapshots, ordered stages, full copy counters, sync/cleanup,
held launcher exit, device claim release, private staging/mount removal and
file-descriptor/descendant audits pass for each job.

Independent host readback verifies both original MBR entries, inactive EFI
partition, unchanged selected core, original NTFS PBR and preserved BPB/backup,
4-KiB cluster geometry, label and clean metadata. All 309 ordinary files match
the source manifest, accounting only for the expected label patch. The complete
bundled UEFI:NTFS payload also matches. Each job reports 534736896 bytes formatted
and 39444524 bytes copied; the main partition starts at 1048576, and the trailing
EFI partition starts at 535804416 with size 1064448 bytes.

BIOS loads the copied GRUB modules/configuration and boots the copied Linux
kernel/initramfs into userspace reading the USB marker. OVMF x64 separately
runs the bundled UEFI:NTFS loader and its NTFS driver, then launches the copied
EFI application from NTFS. Both boots use snapshot media, no direct kernel/initrd
arguments, and preserve the backing-media hash. The reused EFI application's
historical `RUFUS_SYSLINUX_UEFI_OK` marker identifies this test fixture, not a
Syslinux installation. The ordinary FAT32/BIOS GRUB recipe also passes after the
fixture extension. Unknown image versions remain unknown.

This qualifies the compatible fixture on 512-byte logical sectors and 4-KiB NTFS
clusters. It does not certify arbitrary distribution modules/EFI loaders, other
EFI architectures, Secure Boot, physical media or real polkit. The EFI-capable
GRUB/NTFS Qt gate remains until the GUI checkpoint. No Windows customization
qualification is added; native Windows validation remains deferred.

Compact source/binary/payload/firmware hashes, commands, serial logs, manifests,
readback results and cleanup records are retained in parent
`.rufus-test-artifacts/cp21ad`. Generated USB disks, volume copies, ISO/readback/
initramfs trees, firmware copies, builds and extracted test package are removed.
See [the test record](testing-and-checkpoints.md#cp21ad--native-grub-ntfs-with-bios-and-uefintfs-handoffs)
and [the VM recipe](../linux/tests/vm/README.md#grub-iso-ntfs-with-uefintfs-cp21ad).

**Next: CP21ae.** Enable the qualified EFI-capable GRUB ISO MBR/NTFS choice in
Qt. Reuse shared filesystem/extra-partition policy and pass the UEFI:NTFS
reservation into native cluster/geometry selection and the frozen request.
Check ordinary and large-file ISO defaults, manual choice/rescan, BIOS-or-UEFI
target label, confirmation, retained core/source, NTFS progress, failure/cancel
and READY after helper exit. Preserve plain BIOS, MBR/FAT, GPT/UEFI and DD choices;
use existing Widgets fixtures rather than repeating firmware or Windows tests.


## CP21ae — EFI-capable GRUB ISO MBR/NTFS in Qt

Implemented 2026-10-01. Qt now offers the CP21ad-qualified combined route through
the existing GRUB image controller/helper. The former Syslinux-only extra-loader
query is shared as `biosImageUefiNtfs`; it still calls Rufus's unchanged
`GetExtraPartitions`. Eligibility, filesystem refresh, manual selection and the
frozen START request use that same rule. EFI-capable NTFS images reserve the
trailing UEFI:NTFS partition; plain BIOS images do not. No new writer, formatter,
installer, job abstraction or production dependency is added.

Native MBR image geometry now models GRUB for FAT16/FAT32/NTFS, so plain BIOS
NTFS no longer reserves an implicit UEFI payload. Independent 18/19-MiB boundary
checks verify that the reservation changes eligibility only when required.
Other boot/filesystem geometry keeps its previous meaning. Shared filesystem
policy still determines availability/defaults, including NTFS-only large-file
images. Ordinary EFI images retain their GPT default, hybrids their MBR default,
and eligible explicit partition/filesystem choices survive refreshes.

Widgets checks cover BIOS-or-UEFI target/confirmation wording, retained source
and selected core, default/manual 4/8-KiB clusters, original copy percentages,
one helper authorization and unsuccessful/cancelled jobs. READY remains full
and notifies once only after complete formatting/copy, sync/cleanup and helper
exit. Unplugging after success preserves it. ISO remains recommended in the
hybrid dialog; choosing DD leaves the image unchanged. MBR/FAT and GPT/UEFI
regressions retain their original behavior.

**Backlog correction:** the earlier references to pending ext GRUB ISO creation
were incorrect. Original `src/rufus.c:SetAllowedFileSystems` (including the
pre-extraction Git version) and shared `GetAllowedFileSystems` offer only
FAT16/FAT32/NTFS for `BT_IMAGE`. ext choices belong to standalone `BT_GRUB2`.
Do not implement an additional ext ISO route for Windows parity. Standalone
ext2/ext3 remain supported; other Windows-eligible standalone/filesystem features
remain in the parity backlog. Unknown image versions remain unknown.

CP21ad's native BIOS/UEFI handoffs are reused: helper, request boundary,
partition writer, extraction preflight, payload and original policy/boot records
are unchanged. Arbitrary distribution modules/EFI loaders, other EFI architectures,
Secure Boot, physical USB and actual polkit remain unqualified. No Windows
customization or Windows installation qualification is repeated.

See [the test record](testing-and-checkpoints.md#cp21ae--efi-capable-grub-iso-mbrntfs-in-qt).
Compact evidence is retained in parent `.rufus-test-artifacts/cp21ae`; disposable
normal/sanitized builds are removed after export.

**Next: CP22a.** Audit the Windows BIOS/MBR installation path and dual BIOS/UEFI
rules. Identify reusable original MBR/NTFS boot records and shared image policy,
then define the smallest native adapter/component checkpoint before helper or
Qt enablement. Preserve existing x64 GPT Windows creation and the one-helper
lifecycle. Use generated-file/component checks; do not add Windows customization
or installation qualification beyond the agreed single representative test.


## CP22a — Windows BIOS/dual-boot reuse audit

Completed 2026-10-01. [The source audit](linux-windows-bios-reuse.md) maps the
original Windows dispatch, defaults/shortcuts, filesystem/extra-partition policy,
MBR/PBR bytes and native helper gaps. No production code or enabled GUI route
changes. The NTFS adapter already calls Rufus's original standard NTFS writer;
its provenance and BPB/backup preservation are directly reusable. The MBR writer
already embeds original Rufus and Windows 7 bytes; only its NTFS combination
eligibility and subsequent request/helper/Qt dispatch need extension.

Plain Windows BIOS/NTFS omits UEFI:NTFS; EFI-capable dual mode includes it under
shared `GetExtraPartitions`. Rufus MBR is the default, Alt+A selects Windows 7,
and Alt+E changes Windows firmware/partition/filesystem policy. Keep those
choices frozen with the request and preserve one authorization and existing
progress/sync/cleanup/helper-exit behavior. Reuse format/mount/extraction/WIM and
customization code; do not add a second writer or generic Windows API layer.

The first Windows BIOS checkpoint will cover known modern installation images on
NTFS. FAT dual mode needs original Windows FAT32 PBR selection in the existing
adapter, and legacy WinPE needs original setup/config/loader transformations
and masquerading. Windows 7 fallback-loader extraction, other architectures,
Windows To Go and remaining advanced options stay in scope. The audit explicitly
records them; no host-settings inference is introduced.

Normal and ASan/UBSan/LSan component checks confirm reuse: 29 native NTFS PBR
success/refusal/fault cases, 81 original Rufus/FAT32 MBR calls, 81 Windows 7/FAT32
MBR calls and 631 shared policy assertions pass. Four plan-only calls confirm
that Windows NTFS MBR is currently refused, with and without UEFI:NTFS, before
writes. These are existing-component checks, not a Windows media/installation
qualification. No new Windows customization test or real-ISO copy is run.
Evidence is retained in parent `.rufus-test-artifacts/cp22a`; disposable builds
and generated files are removed. See
[the test record](testing-and-checkpoints.md#cp22a--windows-biosdual-boot-reuse-audit).

**Next: CP22b.** Extend and qualify the existing MBR planner/writer for NTFS with
original Rufus and Windows 7 boot code, plain and with UEFI:NTFS. Check exact
boot bytes, active/type/signature fields, extents and unchanged surrounding data,
plus capacity/sector/refusal/write/sync/cancel cases on generated files. Reuse
existing test runners and leave request/helper/Qt enablement for the following
checkpoints in the audit.


## CP22b — Original Windows MBRs on NTFS

Completed 2026-10-01. The existing `PlanMbrImageLayout` now accepts NTFS with
the original Rufus or Windows 7 bootstrap, plain or with trailing UEFI:NTFS.
Only the supported-combination check changes; existing shared geometry, original
boot arrays, serialization, bounded metadata clearing, retained descriptor,
sync and exact readback are reused. FAT, Linux boot modes and source-free native
formats retain their existing contracts. Bootable exFAT and Windows boot code
with ext remain unsupported. No production dependency or writer is added.

Eight generated-file suites exercise both Windows bootstraps and layouts through
the existing image/preparation runners, normally and with sanitizers. Checks
include exact boot bytes and partition fields, independent `sfdisk` inspection,
capacity/sector/cluster boundaries, refusal before writes, unchanged surrounding
data, stale GPT clearing, short I/O, cancellation, corruption and sync errors.
The 2-TiB boundary is plan-only and allocates no large file. This qualifies the
MBR component, not Windows Setup boot or a combined helper job. See
[the test record](testing-and-checkpoints.md#cp22b--original-windows-mbrs-on-ntfs).

**Next: CP22c.** Add and qualify the explicit frozen Windows BIOS/dual request
and source preflight in the existing protocol. Retain the source identity,
Windows bootstrap choice and actual dual-mode reservation. Begin with known
modern NTFS installers, require the BIOS boot-manager marker, validate the EFI
fallback only for dual mode, and reject incompatible settings before authorization.
Regress existing GPT Windows and Linux requests. Helper dispatch and Qt enablement
follow separately; the current GUI does not gain Windows BIOS creation here.


## CP22c — Windows BIOS/dual request and preflight

Completed 2026-10-02. The existing protocol now represents explicit source-backed
Windows MBR/NTFS jobs, with separate versioned BIOS and dual operations. It freezes
the retained source identity, original Rufus/Windows 7 bootstrap choice, Alt+E
policy and actual UEFI:NTFS reservation. The existing format planner selects the
original bootstrap; no writer, provider or production dependency is added.

The existing BIOS preflight accepts known modern Windows 10/11 installation
metadata and the BIOS BOOTMGR marker, reuses shared filesystem/extra-partition
policy, and checks the EFI fallback only when the extra loader is required.
Unknown/incomplete/legacy images, mixed Linux bootloaders, inconsistent settings
and unsupported reservations are refused. Windows customization validation is
shared with the existing GPT path. An enhanced BIOS-only image can still require
no extra partition; an EFI-capable enhanced image must reserve UEFI:NTFS.

Nine selected suites pass normally and under ASan/UBSan/LSan, with 646 Qt cases
and 631 shared policy checks in each build. Generated ISO/UDF Windows 10/11
metadata and both bootstrap choices cover preflight; existing GPT Windows,
format, process, FreeDOS, Syslinux and GRUB requests remain passing. Tests also
verify malformed requests and that the helper refuses these new operations with
ENOTSUP before worker/target access, leaving target bytes untouched. This explicit
guard prevents an incomplete Windows job from entering the older generic NTFS
path. Windows BIOS/dual creation is still disabled in the helper and Qt GUI.

Fixtures establish request/preflight behavior, not Windows Setup boot. No real
USB, actual authorization, additional Windows customization or installation
qualification is run. Compact logs/configurations/hashes are retained in parent
`.rufus-test-artifacts/cp22c`; disposable builds are removed after export. See
[the test record](testing-and-checkpoints.md#cp22c--windows-biosdual-request-and-preflight).

**Next: CP22d.** Integrate the qualified Windows request into the existing NTFS
helper: original MBR/PBR, optional original UEFI:NTFS image, extraction/shared
customization and final sync/cleanup/exit under one authorization. First qualify
component/file and fault behavior; then check simulated USB creation/readback and
native BIOS/EFI handoff with the preferred kernel driver and absent-kernel FUSE
fallback. A generated BOOTMGR handoff fixture should cover the original Rufus MBR
prompt with another bootable disk. Do not add a Windows customization/installation
matrix. Qt dispatch/defaults and Alt+A/Alt+E enablement remain CP22e, after the
combined helper route is qualified.


## CP22d1 — Windows BIOS/dual NTFS helper integration

Completed 2026-10-02. The existing `prepareNtfs` standard-PBR branch now handles
Windows as well as GRUB, while GRUB-core installation remains specific to GRUB.
The existing target acquisition/format/PBR gates recognize explicit Windows
requests. Plain BIOS writes the selected original MBR and standard NTFS PBR;
dual mode also writes the original embedded UEFI:NTFS image with the same frozen
disk signature. Existing NTFS formatting, source retention, extraction/Windows
customization and final sync/private-mount cleanup are reused. No new writer,
provider, controller, production dependency or Win32 compatibility layer is added.

Windows BIOS always runs the shared image preflight inside the helper before
partition writes, even with no customization and Alt+E disabled. This preserves
known-version/BOOTMGR/partition-policy refusals at the privileged boundary.
The CP22c unconditional ENOTSUP guard is removed now that the NTFS flow is
integrated. The existing client recognizes the bootloader phase for plain BIOS,
requires copied data on success and retains completion only after helper exit.

The existing file-backed GRUB helper fixture and runner now cover Windows too;
its production helper body, native file formatters, MBR/PBR/payload writers and
ISO extraction execute unchanged. The fixture uses separate regular disk/volume
files and a directory in place of a native mount. Checks cover exact original boot
bytes, table fields, payload reservation/content, copied metadata/data and phase
order, plus refusal/source/format/PBR/payload/mount/copy/sync/cleanup/cancel faults.
Ten selected suites pass normally and under ASan/UBSan/LSan: 832 Qt cases and
631 shared policy checks in each build, with no failures/skips/diagnostics.
See [the test record](testing-and-checkpoints.md#cp22d1--windows-biosdual-ntfs-helper-integration).

This is helper integration and file/fault/process qualification, not native
combined media/firmware or Windows Setup qualification. Qt Windows BIOS/dual
selection remains disabled. No real USB, actual authorization or additional
Windows customization/installation qualification is run. Compact evidence is
retained in parent `.rufus-test-artifacts/cp22d1`; both temporary builds and their
generated files are removed after export.

**Next: CP22d2.** Qualify the combined production helper on simulated USB with the
preferred kernel NTFS driver and absent-kernel NTFS-3G fallback. Reuse existing
native guest/provider/authorization/readback infrastructure. Check original MBR
and NTFS PBR, retained signature, optional payload, source tree and complete
mount/sync/cleanup/exit, then BIOS/EFI handoff using a generated BOOTMGR fixture.
Cover the Rufus MBR prompt with a bootable second disk without adding a Windows
installation/customization matrix. Enable the existing Qt route/defaults/Alt+A/
Alt+E only afterward in CP22e. FAT dual and legacy parity stay separate work.


## CP22d2a — Native Windows BIOS/dual NTFS creation (2026-10-02)

The existing native GRUB guest recipe now also exercises the production Windows
BIOS/dual request, client and helper. It reuses partition publication, one
simulated authorization, formatter/driver packaging, lifecycle checks and
independent NTFS readback. No production code, writer, controller, dependency or
Windows payload changes were needed.

Four combinations pass normally and under ASan/UBSan/LSan: original Rufus MBR
with kernel NTFS/dual or NTFS-3G/plain BIOS, and original Windows 7 MBR with kernel
NTFS/plain BIOS or NTFS-3G/dual. Generated Windows 11 ISO and Windows 10 UDF-bridge
metadata exercise the retained source/preflight/copy paths. Exact original boot
records, main extent, optional unchanged UEFI:NTFS, six copied-file hashes, clean
NTFS metadata and complete sync/unmount/helper-exit lifecycle pass in all eight
runs. See [the test record](testing-and-checkpoints.md#cp22d2a--native-windows-biosdual-ntfs-creation).

This qualifies creation/readback on simulated USB, not firmware handoff or
Windows Setup. BOOTMGR remains an opaque test marker; WIM content is generated
metadata. Actual polkit and physical USB are not exercised. Qt enablement stays
disabled. Compact evidence is retained in parent `.rufus-test-artifacts/cp22d2a`;
disposable guest data and both builds are removed after export.

**Next: CP22d2b.** Replace the opaque marker with a generated executable BOOTMGR
handoff fixture. Check BIOS loading through the original MBR/PBR, the Rufus MBR
prompt with another bootable disk, and x64 UEFI loading through the original
UEFI:NTFS payload. Keep this separate from Windows Setup/install/customization
qualification. CP22e then enables Qt defaults, dispatch and Alt+A/Alt+E. FAT dual,
legacy Windows and other parity groups remain in scope.


## CP22d2b — Windows BIOS/dual firmware handoff (2026-10-02)

A VM-test-only CMake/NASM target now generates a 72-KiB executable BOOTMGR and
an active second-disk MBR. The existing Windows native recipe optionally boots
the resulting USB through SeaBIOS and the original MBR/NTFS PBR. The fixture
checks the actual 2000:0000 entry, BIOS drive 0x80 and a tail marker beyond the
64-KiB boundary. Original Rufus MBR keypress boots USB; timeout boots the second
disk. Dual mode reuses the existing OVMF/original UEFI:NTFS/x64 application oracle.

All four creation combinations pass normally and under ASan/UBSan/LSan: eight
native jobs, sixteen BIOS boots and four x64 EFI boots. Independent readback,
one simulated authorization, progress/sync/unmount/exit and unchanged snapshot
backing media remain checked. No production code, Windows boot bytes, Windows
build configuration or runtime dependency was changed. See
[the test record](testing-and-checkpoints.md#cp22d2b--windows-biosdual-firmware-handoff).

This proves the generated handoffs, not Windows Setup, Secure Boot, physical
firmware or other architectures. No further Windows customization/installation
qualification is added. The host's updated hwinfo lacks the ABI required by
installed ntfsprogs-plus; an isolated signature-verified matching library was
used for tests, without changing host packages. The normal host formatter needs
a matching dependency or package rebuild before a real NTFS job can run.

**Host workaround, later on 2026-10-02:** rebuilt unchanged ntfsprogs-plus 1.0.0
using its AUR recipe, with only pkgrel changed from 3 to 3.1, and installed it
through pacman. The installed tools now link to libhd.so.26; hwinfo remains
26.0-1.1. Disposable regular-file formatting, geometry/label readback, label
updates, metadata inspection and read-only fsck pass with 4-KiB and 64-KiB
clusters, both before and after installation. No package pin or compatibility
symlink was added. A newer official package can replace this temporary rebuild.
Evidence is in parent `.rufus-test-artifacts/ntfs-hwinfo26-rebuild`.

**Next: CP22e.** Enable the qualified Windows BIOS/dual NTFS route in the existing
Qt flow, preserving Windows defaults, target labels, Alt+A original MBR choice,
Alt+E dual mode, confirmations, progress and READY only after helper exit.
Regress GPT and Linux choices. FAT dual, legacy and other parity remain separate
checkpoints. Evidence is in parent `.rufus-test-artifacts/cp22d2b`; disposable
builds, fixtures and temporary dependency copies are removed after export.

## CP22e — Windows BIOS/dual NTFS in Qt (2026-10-02)

Completed. Known Windows 10/11 installers with root BOOTMGR now offer MBR/NTFS
through the existing Qt selection/controller/helper. EFI-capable Windows images
default to GPT; manual MBR shows BIOS (or UEFI-CSM). Alt+E refreshes controls,
defaults the qualified image to MBR and shows BIOS or UEFI for EFI-capable dual
mode. Explicit GPT remains selectable. Alt+A persists the original Rufus versus
Windows 7 MBR choice. Both shortcuts retain 3.5-second status messages and cannot
change active scans/jobs. Confirmation distinguishes plain BIOS from dual mode;
bootloader status names the partition boot record instead of an absent UEFI loader.

One existing preflight predicate now supplies Windows BIOS eligibility to both
Qt and fresh helper checks. Native cluster selection uses the actual Windows MBR
and shared extra-partition reservation. Shared filesystem/image/progress rules,
original boot bytes, formatting/mounting and one authorized helper are reused;
no new writer, privilege process or progress implementation is introduced. This
checkpoint adds 125 production lines and removes 31 across nine existing files.
Windows builds/sources, bootstrap payloads, privileged job implementation and
CMake target definitions are unchanged.

Seven selected suites pass normally and under ASan/UBSan/LSan: 1,162 Qt cases
per run, including 21 new Windows GUI cases. Four optional unrelated cases skip
(the absent qualified unknown-version Syslinux companion and real Alpine input).
The user's Windows 11 ISO is selected read-only: GPT default, BIOS/dual labels,
NTFS choice, Alt+A and idle READY are checked without START or host-device writes.
Existing Linux ISO/DD choices, GPT, cancellation, sync/cleanup errors and helper-
exit completion regressions pass. No further Windows customization or installation
qualification is added. See [the test record](testing-and-checkpoints.md#cp22e--windows-biosdual-ntfs-in-qt).

Windows FAT BIOS/dual stays gated until its boot records are integrated; GPT
FAT32 WIM splitting remains available by explicitly selecting GPT after Alt+E.
Unknown/older Windows, mixed loaders, unsupported geometry, physical/Secure Boot
and Windows Setup retain their separate qualification/backlog. Evidence is in
parent `.rufus-test-artifacts/cp22e`; temporary builds/media are removed after export.

**Next: CP22f — original Windows FAT boot records.** Extend the existing FAT
adapter with the original Windows FAT32 PE bootstrap and its backup copy, retaining
formatter BPB/volume identity, BIOS drive ID, bounds, cancellation and flush/readback
checks. Qualify generated files and faults first. Then integrate the shared Windows
FAT/dual request/helper layout and split-WIM copying in subsequent small checkpoints
before offering those choices in Qt. Legacy WinPE/Windows 7 stays separate work.


## CP22f — Original Windows FAT boot records (2026-10-02)

The existing FAT adapter now has a Windows record choice: original standard
FAT16 and BOOTMGR FAT32 PE writers, matching `WritePBR`. No bootstrap bytes,
Windows sources/builds, new production files or privilege paths are introduced.
FAT32 stages both copies before device writes. Besides prefixes 0..2 and 6..8,
the original PE bootstrap writes sector 12 and its sector-18 backup. Both tails
must fit inside the reserved area. Transfer/readback covers these four disjoint
regions, preserving the gaps, formatter BPB/serial, FAT tables, directory label
and all other filesystem data. Original BIOS drive-ID/boot-label changes and
PE FSInfo hint-byte resets are retained exactly.

Cancellation, short/interrupted I/O, failed staging/writes, partial-write flush,
sync failures, readback corruption, descriptor ownership/position and file-size
checks reuse the existing adapter. Generated real FAT volumes are checked against
independently loaded original payload arrays, with nonzero reserved gaps and
separate backup hints. This qualifies the record component only; FAT BIOS/dual
requests/helper and Qt choices remain gated until integrated and boot-qualified.

**Next: CP22g — Windows FAT layout and request/preflight.** Qualify the existing
Windows FAT32 MBR planner and extend the frozen BIOS request/preflight, retaining original
MBR selection, EFI files on the FAT volume (no UEFI:NTFS tail), geometry/bounds,
and existing split-WIM policy. Qualify generated layouts and requests before
helper integration. Then reuse the FAT formatter/record adapter, extraction and
split-WIM route in the same authorized helper, qualify the native/firmware
handoff, and enable Qt in subsequent small checkpoints. FAT16 record matching
does not by itself establish modern Windows FAT16 support. Legacy paths stay
separate and in scope. See [the checkpoint record](testing-and-checkpoints.md#cp22f--original-windows-fat-boot-records).


## CP22g — Windows FAT32 layout and request/preflight (2026-10-02)

The existing Windows BIOS request now accepts FAT32 with a distinct versioned
operation, preserving retained ISO identity, FAT volume ID, MBR choice and
Windows settings. UEFI:NTFS is invalid on this FAT route. The existing native
MBR writer/planner already supports the original Rufus/Windows 7 bootstrap on
FAT32; it needs no production change or duplicate serializer. Geometry/cluster
selection uses that same planner, with one active FAT32 partition and no loader
tail reservation.

Shared Rufus filesystem policy still requires Alt+E for Windows BIOS/FAT,
including small installation archives. EFI-capable enhanced FAT media requires
the existing readable x64 fallback EFI application on that volume; no extra
UEFI:NTFS partition is created. BIOS-only source images remain BIOS-only.
GPT and BIOS now share the existing conventional single-install.wim split check,
including archive identity, collision, FAT file-size limit and temporary split
space accounting. Unknown/older Windows and mixed loaders remain refused by the
same source eligibility rule. No new formatting, extraction or split implementation
is introduced.

The helper explicitly refuses Windows FAT before source/target opening or writes;
Qt continues masking Windows BIOS filesystems to qualified NTFS. This checkpoint
qualifies settings/layout/preflight rather than bootable FAT creation. See
[the test record](testing-and-checkpoints.md#cp22g--windows-fat32-layout-and-requestpreflight).

**Next: CP22h — Windows FAT32 helper integration.** Remove the temporary refusal
only when the existing helper installs CP22f's original Windows FAT records after
formatting and before mounting/copying. Reuse retained target identity/MBR checks,
source preflight, FAT formatting/extraction, existing split-WIM copying and final
sync/unmount/worker-exit completion. Qualify generated files/faults first, then the
native/firmware handoff before exposing FAT in Qt. No additional Windows
customization/installation matrix is scheduled. Legacy and FAT16 parity remain
separate work; a matching record/layout is not qualification of those boot paths.


## CP22h — Windows FAT32 helper integration (2026-10-02)

The existing authorized helper now accepts CP22g's Windows FAT32 request. After
native formatting it installs CP22f's unchanged original Windows FAT32 PE records
through the existing FAT adapter, before mounting/copying. The shared FAT boot
operation also retains GRUB's original behavior. It checks the retained disk,
original MBR/bootstrap/signature and partition layout around record installation.
Windows FAT jobs now receive the existing random BIOS disk signature.

Source preflight, formatter handoff, ordinary extraction, optional install.wim
splitting, Windows customization and final sync/unmount/helper-exit completion
reuse their existing paths. One production file changes by +15/-14 lines; no
new writer, payload, privilege path or Windows source/build change is introduced.
Generated-file tests cover both MBR choices, BIOS-only/EFI-capable sources,
ISO9660/UDF copying, actual small-WIM splitting and write/readback/sync/source/
MBR/cancellation/cleanup failures. The mount fixture copies into a separate
ordinary directory; it does not prove a combined native bootable disk. Qt still
masks Windows BIOS/dual filesystems to NTFS. See
[the test record](testing-and-checkpoints.md#cp22h--windows-fat32-helper-integration).

**Next: CP22i — native Windows FAT32 creation and firmware handoff.** Use the
existing simulated-USB/native FAT recipes and generated BIOS/x64 EFI handoff
fixtures to check the combined filesystem, original primary/backup records,
both Windows MBR choices and completion/cleanup. Reuse existing large-WIM split
qualification; do not schedule another Windows installation/customization
matrix. Then enable the qualified FAT32 choice in Qt as CP22j, preserving Rufus's
Alt+E policy, MBR/default choices and final READY behavior. Legacy/FAT16 support,
physical Windows Setup and Secure Boot remain separately tracked.


## CP22i — Native Windows FAT32 creation and firmware handoff (2026-10-02)

The existing Windows/MBR VM recipe now accepts FAT32 as well as NTFS. Generated
Windows 11 ISO and Windows 10 UDF sources exercise both original MBR choices,
BIOS-only and EFI-capable inputs on native simulated USB. The unchanged helper
uses native mkfs.fat, original Windows PE records, kernel vfat mounting, ordinary
copying and final sync/unmount/exit. Independent filesystem readback checks the
complete source inventory and file hashes. SeaBIOS executes the original MBR/PBR
and the generated BOOTMGR; dual media boots the existing x64 EFI fixture directly
from FAT through OVMF, without UEFI:NTFS. Rufus MBR key/timeout choices retain their
second-disk checks. No production or Windows source/payload changes are needed.

Linux updates live FSInfo allocation hints after copying; executable record bytes
and both boot sectors still match the original payloads. The existing strict
checker accepts only the original NO NAME boot-label warning versus the retained
directory label. One authorization launch, retained source, phase/progress order,
held helper exit, exclusive disk reacquisition and complete cleanup pass. This
qualifies generated handoff on 64-MiB media with 512-byte sectors/clusters, not
Windows Setup, physical firmware, other architectures or Secure Boot. Existing
large-WIM splitting and the sole customization qualification are reused.
See [the test record](testing-and-checkpoints.md#cp22i--native-windows-fat32-creation-and-firmware-handoff).

**Next: CP22j — Windows FAT32 BIOS/dual in Qt.** Remove only the temporary NTFS
mask for the qualified modern Windows FAT32 route. Retain shared Rufus Alt+E
filesystem eligibility, exact Win32 defaults/MBR choices, source preflight,
cluster/partition policy, confirmation and final READY after helper exit. Add
focused selection/request GUI checks before exposing the choice. Keep FAT16,
legacy Windows/WinPE, Windows To Go and other remaining parity separately tracked.


## CP22j — Windows FAT32 BIOS/dual in Qt (2026-10-02)

Qt now exposes the modern Windows FAT32 route qualified by CP22f–i. The existing
selection controller admits FAT32 alongside NTFS and chooses the original
Rufus/Windows 7 MBR for either. Confirmation uses the frozen enhanced-install
flag to describe direct FAT EFI boot as BIOS or UEFI. These are the only two
production files changed: +4/-3 lines. The helper, formatter, boot records,
request protocol, split-WIM path and original Windows sources are unchanged.

Shared Rufus policy still requires Alt+E for Windows BIOS FAT32. A valid small-WIM
FAT32 selection survives refresh; a large-WIM image keeps NTFS until FAT32 is
chosen explicitly. Disabling Alt+E restores automatic GPT selection, or falls
back to NTFS for an explicitly selected MBR. Alt+A retains the original MBR
choice. Native cluster eligibility, GPT/UEFI, existing NTFS and ISO/DD behavior,
request freezing and final READY after helper exit retain their existing paths.

Fourteen additional Qt scenarios cover selection/request/default/confirmation
and sync/cleanup/cancel/held-exit behavior. Seven suites pass normally and under
ASan/UBSan/LSan, including read-only selection of the supplied Windows 11 ISO.
Generated GUI jobs use existing private D-Bus/regular-file helper fixtures;
CP22i supplies native FAT and firmware evidence. No additional Windows
customization or installation qualification is scheduled. See
[the test record](testing-and-checkpoints.md#cp22j--windows-fat32-biosdual-in-qt).

**Next: CP22k — legacy Windows/WinPE reuse audit.** Trace original filesystem,
MBR/PBR, copying and source-version decisions for older BOOTMGR installers and
legacy WinPE. Identify what the existing shared writers and Linux adapters
already cover, then select one bounded implementation checkpoint with explicit
source prerequisites. Keep unknown versions and mixed loaders gated until that
route is established; do not simply relax the Windows 10/11 predicate. FAT16,
Windows To Go and other remaining parity stay tracked separately.


## CP22k — Legacy Windows/WinPE reuse audit (2026-10-02)

[The detailed audit](linux-legacy-windows-reuse.md) separates older BOOTMGR
installers from Windows 7-style EFI repair and XP-era WinPE. Known Windows
7/8/8.1 plain BIOS/NTFS can reuse both original MBRs, the existing standard NTFS
PBR, formatter/mount, ISO/UDF extraction and one-helper completion. Shared WIM
metadata already recognizes these versions. No new bootloader, formatter or
Windows API compatibility layer is needed for that first route.

Windows 7-style EFI needs the original first-install-image bootmgfw.efi extraction
when the exact HAS_WIN7_EFI source trigger applies. Legacy WinPE needs shared
SETUPLDR/SIF transformations and original Rufus-MBR active byte 0x81, which the
current Linux serializer/readback does not implement. FAT32's original NT and PE
records search for different loaders; standard FAT16 searches for DOS files.
Keep complete-chain qualification separate from boot-record byte provenance.

Four existing metadata/WinPE suites pass, including generated ISO/UDF inputs and
unknown-version/pending/cancellation checks. All 690 production hashes remain
identical to CP22j. This checkpoint adds an audit and a concrete implementation
split; it enables no new GUI/helper route. Test evidence is recorded in
[the checkpoint log](testing-and-checkpoints.md#cp22k--legacy-windowswinpe-reuse-audit).

**Next: CP22l — older BOOTMGR plain BIOS/NTFS preflight/helper qualification.**
Admit known normalized 7.0/8.0/8.1 install metadata with root BOOTMGR and no mixed
or legacy loader through the existing Windows request. Initially keep enhanced/
EFI/FAT and Qt enablement separate. Extend the existing small WIM/ISO/UDF and
helper fixtures, then qualify native readback/generated BIOS handoff before
exposing the route in Qt. Unknown versions stay refused. Follow with the exact
Windows 7 EFI operation and shared WinPE transformations/0x81 support in their
own checkpoints. Reuse large-WIM and sole customization evidence.
Reuse existing NTFS-provider/both-MBR qualification and use one representative
older-source native creation/handoff, with version/refusal combinations in
focused fixtures, rather than a new per-release/provider installation matrix.


## CP22l1 — Older BOOTMGR plain BIOS/NTFS preflight and helper (2026-10-02)

Known normalized Windows 7.0/8.0/8.1 now passes the existing plain Windows
BIOS/MBR/NTFS backend preflight without Alt+E. Complete analysis, install metadata,
root BOOTMGR and absence of legacy/mixed loaders remain required. Unknown versions,
enhanced/dual, FAT, GPT and unavailable customization requests remain refused
before writes. Qt retains the modern-only default gate pending native qualification.
Only `iso_write_check.cpp` and its header change (+15/-7 lines); existing request,
helper, original MBR/NTFS PBR, formatter/mount, copying and cleanup are unchanged.

Extend existing fixtures: 41 preflight rows, 24 helper rows, two Qt gate rows and
four Windows scanner probes. Five backend suites and focused Windows Qt checks
pass normally and under ASan/UBSan/LSan. The supplied Windows 11 ISO is selected
read-only. Generated helper tests use separate regular-file record slices and a
directory mount stand-in; they do not qualify native combined media or firmware.
See [the test record](testing-and-checkpoints.md#cp22l1--older-bootmgr-plain-biosntfs-preflight-and-helper).

**Next: CP22l2 — representative older-source native NTFS creation and BIOS
handoff.** Reuse existing simulated USB, original records, native formatter/mount,
independent readback and generated executable BOOTMGR through the backend request.
Use one representative older source with both original MBRs, reusing existing
NTFS-provider qualification rather than a per-release/provider installation
matrix. Keep Qt gated until this passes, then enable only the qualified plain
BIOS/NTFS choice in CP22l3. Older EFI/FAT, Win7-style repair and legacy WinPE/0x81
remain separate. No more customization qualification is scheduled. Stop for the
day after exporting evidence and deleting both disposable builds.


## CP22l2 — Older-source native NTFS creation and BIOS handoff (2026-10-03)

One representative Windows 7-style source passes native simulated-USB creation,
independent readback and generated BIOS handoff through the unchanged backend.
The existing VM recipe accepts version 7 for plain NTFS only. Its source has
known install metadata, root BOOTMGR and bootmgr.efi without a fallback EFI
application. Plain BIOS copies those files unchanged and does not perform
Windows 7-style EFI repair, matching the original target/Alt+E condition.

Four jobs pass: Rufus MBR/kernel NTFS/ISO and Windows 7 MBR/NTFS-3G/UDF bridge,
normally and under ASan/UBSan/LSan. Independent readback checks original records,
geometry/label/clean metadata and complete file inventory/hashes. Eight SeaBIOS
boots execute original MBR/PBR and generated 72-KiB BOOTMGR, including Rufus's
key/timeout second-disk choices. Each job retains one simulated authorization,
source identity, progress, held-helper-exit ordering and full sync/unmount/child/
descriptor cleanup. All 690 production hashes are unchanged; only two VM recipe
files change. See [the record](testing-and-checkpoints.md#cp22l2--older-source-native-ntfs-creation-and-bios-handoff).

This establishes representative native creation/handoff, not actual Windows
Setup/BCD, physical firmware or an installation matrix. CP22l1 covers 7/8/8.1
version/refusal combinations. Reuse existing provider/both-MBR, large-WIM and
sole customization evidence. Export compact evidence and remove disposable media,
guest-output duplicates and temporary builds.

**Next: CP22l3 — older BOOTMGR plain BIOS/NTFS in Qt.** Admit known 7.0/8.0/8.1
with qualified BIOS/MBR/NTFS choices, original defaults and both MBR options.
Keep GPT/UEFI, Alt+E, FAT, Win7-style repair and legacy WinPE gated until their
own implementation/qualification. Check ISO load, refresh, request/confirmation
and completion against shared Rufus policy without another creation flow or
older customization options.


## CP22l3 — Older BOOTMGR plain BIOS/NTFS in Qt (2026-10-03)

Qt now admits known normalized Windows 7.0/8.0/8.1 installers through the
qualified ordinary Windows BIOS request. Complete known metadata, root BOOTMGR,
no mixed/legacy loader and native MBR geometry remain prerequisites. Both MBR
choices, shared filesystem/extra-partition rules, preflight, confirmation,
progress and READY reuse existing code. No new helper or creation flow is added.
Three Qt production files change (+26/-10 lines); the preflight header also
clarifies its existing explicit older-installer allowance (+2/-2 comments).

These sources offer only MBR/NTFS/BIOS, including images with EFI files. Windows
Rufus supports further older EFI/FAT and the Win7-style payload repair; Linux
has not implemented/qualified them yet. This is a temporary capability limit.
It avoids offering GPT/EFI choices that preflight cannot execute. A saved Alt+E
preference is retained for other images but not applied to this request; pressing
it explains the limitation without changing that preference. Alt+A still selects
either original MBR. Original customization rules offer no Windows User Experience
or edition chooser for these older installers. Unknown versions remain refused.

Thirty-six additional Qt scenarios cover all three versions, BIOS/root-EFI/
fallback-EFI source layouts, both MBRs, saved preferences, device removal and
refresh, MBR capacity refusal, source-free/image/modern-image transitions,
confirmation/request and sync/cleanup/cancel/held-exit completion. Existing modern
Windows and Linux regression suites run normally and with ASan/UBSan/LSan.
See [the record](testing-and-checkpoints.md#cp22l3--older-bootmgr-plain-biosntfs-in-qt).
CP22l1/2 supplies component/native readback/generated handoff evidence; actual
older Windows Setup/BCD remains unverified. No new customization matrix is added.

**Next: CP22m1 — Windows 7-style EFI payload preflight.** Follow the original
HAS_WIN7_EFI source-content trigger, rather than testing only version 7. Locate
image 1 of the first install archive and its exact
`Windows/Boot/EFI/bootmgfw.efi` payload through retained ISO/UDF/upstream-wimlib
reading. Reuse existing native WIM/boot-file operations and check a usable x64
EFI application before planning fallback installation. Missing/wrong/unreadable
payloads must remain refused; do not copy root bootmgr.efi as a substitute or
change unknown version reporting. Helper fallback installation and native EFI
handoff follow in separate checkpoints before widening Qt EFI/FAT choices.
Legacy WinPE shared transformations/0x81, FAT16 and other parity remain tracked.


## CP22m1 — Windows 7-style EFI payload preflight (2026-10-03)

Completed the bounded read-only lookup. `ReadWindowsEfiRepair` uses the original
`HAS_WIN7_EFI` content trigger, first recorded install archive, image 1 and exact
`Windows/Boot/EFI/bootmgfw.efi` path. It borrows the retained ISO/UDF view and
upstream wimlib reader, reuses bounded `ReadWimFile`, and returns owned application
bytes. Root bootmgr.efi, boot.wim, the archive boot index, later images and later
archives cannot substitute. The source report and unknown version remain intact.

Existing fallback-application checks now share the same bounded PE/x64/EFI
validation with the WIM payload. No new PE parser, metadata policy, request field,
privilege flow or helper write route is added. The reader's existing 16-MiB file
limit applies; this is structural validation, not firmware execution or trust
validation. Caller serialization, source lifetime and final source-change checks
remain required. Cancellation/failure clears payload ownership.

Thirty-one new component cases and the five affected suites pass normally and
with ASan/UBSan/LSan. Qt and backend route gates are unchanged. See
[the record](testing-and-checkpoints.md#cp22m1--windows-7-style-efi-payload-preflight).

**Next: CP22m2 — install the verified fallback in the existing helper.** Connect
this lookup to fresh helper preflight only for the original UEFI/Alt+E repair
condition. Retain the checked payload from the selected source across normal
copying/WIM splitting; no copied install archive or second archive search should
be needed. Reuse existing atomic boot-file installation and private mount/cleanup
operations to create `EFI/BOOT/BOOTX64.EFI` after copying, with cancellation and
final sync-before-READY preserved. Component/helper tests must cover missing or
invalid payloads before writes, exact installed bytes and error/cancel cleanup.
Keep older EFI/FAT Qt choices gated until representative native readback and
generated EFI handoff qualify the combined route. This is separate from actual
Windows Setup/BCD, Secure Boot, other architectures, legacy WinPE/0x81 and FAT16.
Do not add a Windows installation/customization matrix.


## CP22m2 — EFI repair payload installation in the helper (2026-10-03)

Fresh ISO preflight now obtains the CP22m1 payload for the original UEFI/Alt+E
content trigger, accounts for its additional bytes and retains ownership across
copying/WIM splitting. Plain BIOS/NTFS still copies unchanged without repair.
Known-version, older enhanced/UEFI/FAT and Qt eligibility gates are unchanged;
the integrated preflight tests use a qualified modern Windows version with the
original root-EFI-only content layout. Unknown versions remain refused.

The existing helper performs the repair after copying/source verification and
before customization/final sync. Every non-Syslinux ISO path now runs fresh
image policy; Syslinux already checks its source separately. This closes the
previous ordinary-UEFI path that only checked the extraction tree, so a required
missing/invalid payload is refused before partition writes. Payload bytes come
only from fresh source reading, with no client field or copied-WIM reopening.
The validated x64 payload supplies the repair architecture rather than host
inference. No additional phase/progress slot or authorization is introduced;
repair remains within copying before existing customization/finalization.

`InstallWindowsEfiRepair` creates EFI/BOOT through directory descriptors and uses
the existing atomic boot-file writer for BOOTX64.EFI. The shared path accepts
QFile or QBuffer, retaining 64-KiB batching, short-write/cancellation checks,
explicit fsync and QSaveFile replacement. Links/non-files are refused. Error
and cancellation keep an existing fallback intact before commit; directory
creation/whole USB preparation is not a transaction. The caller still owns
filesystem/device sync and private mount cleanup before successful READY.

Thirty-nine new cases pass, including real fixture copying followed by removal
of the copied archive and installation of exact retained bytes. Five component/
request/process suites pass normally and under ASan/UBSan/LSan. Original Windows,
Qt, protocol, formatting, mount and reader code remains unchanged; production
source changes are five files, +108/-23 lines. See
[the record](testing-and-checkpoints.md#cp22m2--efi-repair-payload-installation-in-the-helper).

**Next: CP22m3 — native creation/readback and generated EFI handoff.** Exercise
this complete helper path on simulated USB, using the root-EFI-only source
content trigger with a currently qualified known version. Independently inspect
the installed fallback and unchanged source files on representative FAT32 and
NTFS/UEFI:NTFS creation, then execute the generated EFI application in OVMF.
Keep source identity, one authorization, progress/held-helper-exit ordering,
sync/unmount and failure/cancel cleanup checks. Reuse existing provider/large-WIM
and sole customization evidence rather than repeating matrices. Actual Windows
Setup/BCD/Secure Boot, older version admission and Qt EFI/FAT enablement follow
separately; no new Windows customization/installation matrix is planned.

## CP22m3 — native EFI repair creation/readback/handoff (2026-10-03)

The existing complete-helper VM harness now supplies the generated x64 EFI
application inside image 1 of install.wim, with a distinct root bootmgr.efi and
no EFI/BOOT fallback in the ISO tree. Known Windows 11 fixture metadata exercises
the original content-driven repair trigger without changing older eligibility.
No production code changes are needed for this checkpoint.

FAT32/ISO and preferred kernel NTFS/UDF creation pass normally and under
ASan/UBSan/LSan on simulated USB. Independent filesystem readback verifies the
original files byte for byte and exactly one added EFI/BOOT/BOOTX64.EFI, matching
the application supplied to the WIM. Original MBR/PBR records, filesystem state
and the NTFS route's original UEFI:NTFS partition are checked. OVMF executes the
repaired loader directly on FAT32 and through UEFI:NTFS on NTFS, without changing
the backing disk. This proves generated EFI handoff, not Windows Setup/BCD or
Secure Boot. Existing BIOS, provider, large-WIM and sole customization evidence
is reused.

The same guest checks one simulated authorization, retained source identity,
phase/copy-progress ordering, final sync/unmount, private-resource cleanup and
waiting for the held launcher to exit before completion. Repair stays within
copying and introduces no extra prompt or phase. CP22m2's repair-specific fault/
cancellation cases and existing native copy/finalization failure checks remain
the error-path evidence; this checkpoint adds no new native fault matrix.
The Windows image and VM supervision regressions also pass. See
[the record](testing-and-checkpoints.md#cp22m3--native-efi-repair-creationreadbackhandoff).

**Next: CP22m4 — bounded older EFI/FAT eligibility.** Audit the known older
BOOTMGR source policy and permit only routes supported by the checked x64
fallback and original boot records. Preserve refusal for unknown versions and
unsupported WinPE/mixed loaders. Qualify request/preflight behavior before
widening the Qt filesystem/target/Alt+E choices, then qualify an older generated
native route. Reuse these same helper/EFI fixtures; no additional Windows
customization or installation matrix is planned.

## CP22m4 — bounded older EFI/FAT eligibility (2026-10-03)

Backend preflight now admits known normalized Windows 7.0/8.0/8.1 installers
for FAT32/NTFS UEFI and enhanced BIOS routes, using the same native operations
as modern Windows. BIOS still requires BOOTMGR; UEFI requires bootmgr.efi and a
checked x64 fallback, either in the tree or repaired from the first install
image. A UEFI-only source need not contain the BIOS manager. The small existing
known-version test is shared by the BIOS and UEFI predicates. No source version
is inferred or reclassified, and no new formatter, boot writer or helper path
is introduced.

Shared Rufus filesystem/partition rules remain authoritative: plain BIOS FAT32
is refused, Alt+E permits FAT32, EFI-capable enhanced NTFS requires UEFI:NTFS,
and a single conventional oversized install.wim can use enhanced FAT splitting.
Unknown/incomplete metadata, legacy WinPE/minint and older mixed loaders remain
refused. Older FAT16 remains a separately tracked capability limit. Original
customization policy still offers no options/edition chooser for these versions.
Modern UEFI behavior and existing BIOS predicates are preserved.

Qt retains plain older BIOS/NTFS for now. Regression testing found that an older
EFI-only source previously relied on the backend's version refusal and could
reach confirmation after eligibility widened. An explicit check in the existing
Qt preflight-result handler preserves that limit, including saved Alt+E, until
native qualification and intentional GUI exposure. This is a temporary staging
limit, not a new creation flow or a permanent departure from Windows Rufus.

Eighty-seven additional preflight scenarios and five additional Qt cases pass.
Four complete component/request/process suites and seven focused Qt behaviors
pass normally and with ASan/UBSan/LSan: 637 QTest passes per build, no failures,
skips or sanitizer findings in the final runs. Production changes are only
three files, +33/-18 lines; 687 other recorded production hashes are unchanged.
See [the record](testing-and-checkpoints.md#cp22m4--bounded-older-efifat-eligibility).

**Next: CP22m5 — older native EFI creation/readback/handoff.** Reuse the CP22m3
generated first-install-image application with known Windows 7 metadata on
representative FAT32/ISO and NTFS/UDF native helper jobs. Verify exact source/
fallback files, original records, direct/UEFI:NTFS OVMF handoff and one-helper
sync/cleanup/exit before widening Qt target/filesystem/Alt+E choices. Reuse
existing GPT geometry, providers, large-WIM, fault/cancel and sole customization
evidence. Actual Windows Setup/BCD/Secure Boot stays separate; no new Windows
installation/customization matrix is planned.

## CP22m5 — older native EFI creation/readback/handoff (2026-10-03)

Known Windows 7 metadata now passes the complete native repair route on simulated
USB: FAT32/ISO and preferred kernel NTFS/UDF, each normally and with
ASan/UBSan/LSan. The first install image supplies the generated x64 application;
root bootmgr.efi remains distinct and the source tree contains no fallback.
Independent readback verifies all original files and exactly the required added
EFI/BOOT/BOOTX64.EFI. Original MBR/PBR bytes, filesystem state and the NTFS
route's original UEFI:NTFS partition also match. OVMF executes the repaired
loader directly on FAT32 and through UEFI:NTFS on NTFS, without changing the
backing media.

Existing source retention, phase/copy-progress ordering, one simulated
authorization, final sync/unmount, private-resource/child/descriptor cleanup and
held-launcher-exit checks all pass. All 690 production hashes match CP22m4;
only two test recipe guards change. The ordinary older BIOS recipe still requires
no added fallback; the explicit repair recipe now expects it. Reuse CP22m4's
three-version/regular-fallback/request/preflight qualification and existing BIOS,
GPT, providers, large-WIM, error/cancel and sole customization evidence. This
generated handoff does not qualify actual Windows Setup/BCD or Secure Boot.
See [the record](testing-and-checkpoints.md#cp22m5--older-native-efi-creationreadbackhandoff).

**CP22m6 scope — older EFI/FAT32 choices in Qt.** Intentionally widen the known
7.0/8.0/8.1 filesystem/target/Alt+E choices, using shared Rufus defaults and
partition rules. Remove the temporary GUI limit only for these qualified routes.
Cover ordinary/regular-EFI/root-EFI-only sources, saved preferences, both MBR
choices, refresh, confirmation/request and completion. Preserve unknown/legacy/
mixed-source refusals and separately tracked older FAT16 limits. No new helper,
host inference or customization/installation matrix is needed.


## CP22m6 — Older EFI/FAT32 choices in Qt (2026-10-03)

Known Windows 7.0/8.0/8.1 now use the qualified shared FAT32/NTFS BIOS,
enhanced and x64 UEFI paths in Qt. The temporary older BIOS/NTFS-only controller
result guard and version-based `windowsBiosOnly()` method are removed. The
saved Alt+E preference applies normally and persists through image/device refresh.
Both original MBRs remain selectable with Alt+A, with shortcuts blocked while
scanning, checking or creating.

The Windows `SetPartitionSchemeAndTargetSystem` audit uses `IS_EFI_BOOTABLE`,
not just `HAS_REGULAR_EFI`: root-only Windows EFI layouts are eligible for GPT
and dual target text after the existing image-1 repair preflight. Qt now follows
that content rule for older and modern installers. EFI-capable installers default
to GPT unless an explicit supported partition choice exists; Alt+E defaults
BIOS-capable installers to MBR and permits FAT32. BIOS-only installers remain
MBR/BIOS. Explicit GPT after Alt+E remains available. Selection and confirmation
use the same EFI predicate; NTFS UEFI retains original UEFI:NTFS. UEFI-only
sources do not acquire a BIOS route without BIOS BOOTMGR.

Older FAT16 is still separately unqualified and hidden on both MBR/GPT. Fresh
backend checks retain unknown-version, legacy/mixed-source, x64 loader/repair,
geometry and single-large-WIM policy. Older images do not gain the modern Windows
User Experience or edition chooser. Existing helper, authorization, progress,
final sync/unmount/exit and completion behavior are reused without new writers.

The synthetic GUI fixture now places its structural x64 application at the exact
WIM repair path only for root-only layouts; it does not include that payload as
an extra ISO source file. Consolidated selection/request data replaces duplicate
older plain-BIOS tests and covers all five known versions, three layouts, both
MBRs, Alt+E and small/large-WIM FAT32. UEFI-only older sources, saved preferences,
refresh and completion/error/cancel are covered separately. Native readback/
firmware qualification remains CP22l/m; no new customization/installation matrix
or physical-media write is added. Actual older Setup/BCD/Secure Boot is unverified.
Seven full suites pass normally and sanitized: 1,605 checks and one optional
Alpine-input skip in each build, with no failures/findings. The initial full run
caught an overbroad FAT16 mask; the fixed mask only applies to known older
installers in image mode, preserving modern UEFI/source-free formatting choices.
The production delta is +20/-39 lines across five files, with 685 other production
files unchanged. About 1.03 GiB of temporary builds/fixtures is removed after
exporting compact evidence. Full details are recorded in
[the checkpoint log](testing-and-checkpoints.md#cp22m6--older-efifat32-choices-in-qt).

**CP22n1 scope — shared legacy WinPE transformations.** Follow the existing
CP22k reuse audit: isolate Rufus's SETUPLDR/SIF transformations into shared,
GUI-free code with bounded fixture tests, retaining the original Windows file
operations and build. Establish the exact AMD64/minint/i386 selection, byte
patches and error behavior before connecting native copying or enabling Qt.
Original 0x81 MBR masquerading and complete native legacy-chain qualification
remain separate steps; Windows BIOS/dual FAT16 and Windows To Go remain tracked.
No new host inference or additional Windows customization qualification is needed.


## CP22n1 — Shared legacy WinPE transformations (2026-10-03)

`src/winpe_setup.c/.h` now owns SetupWinPE's layout/copy decisions and loader
buffer transformations. Windows calls those shared operations while retaining
its existing CopyFile, SIF insertion, handle, allocation, retry-write and cleanup
operations. AMD64 takes loaders from i386; complete AMD64 precedes complete minint
for directory selection. Any minint bits retain the original keep-loader early
exit with /minint, or failure without it, including mixed/partial masks. Copying
SIF still occurs before that early exit when /minint is absent. This is distinct
from the existing options scanner's i386-first priority, which is unchanged.

The shared patch retains the exact Win2k3 CRC signature/offset, case-insensitive
prefix matching with the final pattern character omitted, path replacement/NUL
writes, first-pass start at byte 1, excluded 32-byte tail, inner-loop advancement,
rdisk replacement and following-character behavior, and /minint's suppression
of the second pass. Existing `uprintf` reporting is reused. No callbacks or Qt,
Windows API, file I/O or allocation are added to these operations.

An actual existing short-file safety defect is fixed: unsigned `size - 32` no
longer underflows. Inputs of at most 32 bytes are no-ops, invalid arguments fail
without mutation, and each comparison/write plus unterminated logging is bounded
by the owned buffer. These bounds also protect the original Windows caller;
well-formed fixture bytes match the original loops exactly.

The section match from `parser.c:insert_section_data` is now
`config_text.c:IsConfigSection`, called by Windows's unchanged insertion loop.
It skips only leading spaces/tabs, compares a case-insensitive section prefix,
and still allows trailing text/repeated matching chunks. Shared constants retain
the exact SetupData section and SetupSourceDevice value. Encoding/BOM, 1024-wide
character chunks, newline conversion, insertion/output and file replacement stay
with native I/O; no general INI parser or complete Linux SIF writer is added yet.

The existing Windows Makefile and Visual Studio manifests include the shared
source/header; Linux adds a CMake component target. Original Windows build/runtime
validation remains deferred as agreed. Component tests use explicit byte
expectations and a test-only copy of the baseline loops for well-formed cases;
the reference is never run on malformed/short inputs or shipped in production.
All 64,996 loader/layout checks and 336 configuration checks pass normally and
with ASan/UBSan. Four integrated Linux suites pass, including 43 config-file
adapter and 409 ISO-preflight cases. The Qt GUI and helper rebuild successfully. Five temporary builds are removed
after export (178.6 MiB); user ISO/dependency sources are preserved.
No new Linux route or customization/installation test is enabled. See
[the record](testing-and-checkpoints.md#cp22n1--shared-legacy-winpe-transformations).

**Next: CP22n2 — native WinPE root-file preparation component.** Use the shared
plan and patches to prepare ntdetect.com, BOOTMGR and, where required, root
TXTSETUP.SIF in an owned temporary-directory fixture before connecting the helper.
Retain source files and Windows-style unambiguous case matching on Linux;
investigate/preserve original SIF encoding/chunk/newline semantics, and treat missing mandatory files, unsupported layouts, unknown options,
source changes, cancellation and output/verification errors explicitly. This is
not a new privileged worker or GUI enablement. The original 0x81 MBR masquerading,
legacy NTFS/FAT32 records, native discovery/mounting and generated BIOS handoff
remain later checkpoints before exposing the route. Older FAT16/Windows To Go and
other parity work remain tracked; no new host inference or customization matrix.

## CP22n2 — Native WinPE root-file preparation component (2026-10-03)

`linux/platform/winpe_prepare.cpp` now prepares the root ntdetect.com, TXTSETUP.SIF
when needed, and BOOTMGR from an owned extracted-directory fixture. It calls the
shared SetupWinPE plan and SETUPLDR patches rather than reproducing their rules.
AMD64 still uses i386 loaders; minint retains the shared keep/refuse decision.
Unknown options/incomplete layouts remain refused. No helper or Qt route is
enabled by this component checkpoint.

Source names are matched case-insensitively with ambiguous matches refused.
Retained directory/file descriptors and identity, size, mtime/ctime checks detect
replacement or modification; links/special/empty files are refused. Inputs and
text transforms are checked before any writes; existing destinations are refused
without overwriting them. Each new file uses a same-directory temporary, bounded
writes/readback with size/content verification, file sync, no-replace atomic
rename and directory sync. Later failures may retain earlier complete outputs;
the job must abort and perform its normal final cleanup, never report READY.
The component is bounded to 8 MiB per source/output file and does not establish
that an arbitrary loader is executable or that WinPE boots.

`config_patch.cpp` shares its existing decoder with a distinct section-insertion
operation, calling `IsConfigSection`. It models the original 1023 UTF-16-unit
chunks, space/tab and case-insensitive prefix matching, repeated insertions,
CRLF input collapse/output expansion, bare CR retention, and insertion directly
after an unterminated section chunk. ISO token replacement retains its existing
LF behavior. ASCII without BOM and valid UTF-8/UTF-16LE with BOM are covered;
unknown non-ASCII no-BOM code pages, malformed/control-marker text and missing
required SetupData sections fail explicitly. Encoding without a BOM must not be
guessed from host settings. The CRT-dependent Windows encoding/BOM round trip
still requires the deferred real Windows validation; the Linux fixtures are not
a substitute for that check.

The native changes deliberately refuse mandatory-copy/SIF errors that original
SetupWinPE sometimes logs and tolerates, and reject unsupported layouts before
copying. Otherwise we could report a completed but incomplete boot chain or
modify outputs for an already-refused layout. This follows the agreed READY and
source-preservation rules; it is not an implementation convenience. No source
file is edited and no general INI parser, privilege flow or host inference is
added. Only two existing inventoried production files change (config adapter
and header), with 690 others unchanged and two new native component files.

All 74 native fixture cases, 43 existing config adapter cases, 409 ISO-preflight
cases, 64,996 shared loader checks and 336 shared config checks pass normally and
with ASan/UBSan, without findings. GUI/helper builds pass. Compact evidence and
cleanup are recorded in
[the checkpoint log](testing-and-checkpoints.md#cp22n2--native-winpe-root-file-preparation-component).

**Next: CP22n3 — legacy WinPE MBR/record preparation component.** Reuse Rufus's
original record providers and policy for the WinPE BIOS case, including the
original 0x81 partition masquerading when /minint is absent, before integrating
root preparation with the existing authorized helper. Keep source analysis,
mounting/formatting, generated BIOS handoff qualification and Qt enablement as
subsequent checkpoints. WinPE bootability, localized no-BOM SIF encoding,
older FAT16/Windows To Go and actual Windows runtime validation remain work;
no further Windows customization qualification or host inference is scheduled.

## CP22n3 — Legacy WinPE boot-record component (2026-10-03)

The existing shared WinPE plan now reports the original WriteMBR masquerading
predicate: any complete WinPE family, without /minint. Windows WriteMBR calls
that field while retaining its original ordering, MBR provider selection,
0x80/0x81 write and Windows I/O. Component checks compare the predicate against
independent original masks across 1,024 option/mask combinations. No #ifdef,
callback bridge or alternate Windows build is added.

The native MBR writer adds an explicit original-Rufus-with-0x81 variant. It
shares the existing bootstrap, libfdisk layout, signature, write/flush/readback
and cancellation operations; ordinary modes retain their bytes and 0x80 policy.
Readback requires the exact raw flag. A direct libfdisk audit on a disposable
file established that its label verification accepts 0x81, but its "bootable"
property is true only for 0x80. The in-memory prewrite table must still be active;
only the stored masquerading variant expects the false library property after
the raw 0x81 check. No label verification or extent/type checks are skipped.
Inactive 0x00, ordinary 0x80 and other-drive 0x82 corruption remain errors.

The existing FAT writer adds the original legacy WinPE provider:
`write_fat_32_nt_br` and its original matcher. It reuses the BOOTMGR writer's
bounded prefix/sector-12-tail staging, backup copy at sector 6, reserved-area
checks, partial-write flushing and full record verification. FAT16 remains the
original standard record for this choice. Source routing must preserve
WritePBR's BOOTMGR-before-WinPE priority; this component does not infer a provider
from a client field. NTFS already uses the original standard NTFS provider,
unchanged. No new record payload, file-I/O engine, helper, mount or Qt route.

**Integration finding:** the bundled legacy FAT32 NT record contains the 8.3
lookup name `NTLDR`, whereas SetupWinPE copies the prepared loader to `BOOTMGR`.
The BOOTMGR FAT32 and standard NTFS records contain `BOOTMGR`. No further root
NTLDR installation was found in the audited SetupWinPE/extraction path. This is
a source/payload discrepancy, not a qualified claim of a Windows runtime bug:
existing image contents and the higher-priority BOOTMGR case can matter. Keep
legacy FAT32 creation gated and exercise the actual BIOS handoff before adding
an alias or changing a Linux provider. Do not silently reinterpret all legacy
images or claim bootability from record-byte agreement.

All 18 focused suites have passing normal/sanitized evidence after correcting
one authored non-bootable fixture expectation. The new record paths cover 474
creation/preparation/record/fault calls, and the shared loader/masquerading suite
passes 66,020 assertions. Existing root preparation (74 Qt cases), config
adapter (43) and ISO preflight (409) pass; GUI/helper builds succeed. Seven of
694 inventoried production files change, with 687 unchanged, no new production
files and no new I/O engine. Windows build/runtime and actual WinPE bootability
remain unqualified. See
[the record](testing-and-checkpoints.md#cp22n3--legacy-winpe-boot-record-component).

**Next: CP22n4 — generated legacy BIOS record-to-loader handoff.** Use bounded
regular-file fixtures and generated non-Windows payloads to test the original
Rufus MBR/0x81 behavior and loader filename/drive-number handoff. Resolve the
FAT32 discrepancy from observed execution; reuse the NTFS BOOTMGR path where
compatible. Only then connect validated source/options, root preparation and
records to the existing single authorized helper, followed by full native
creation/readback/handoff and Qt exposure. Older FAT16, localized no-BOM SIF,
Windows To Go and real Windows runtime validation remain tracked. No host
inference or additional customization qualification is scheduled.

## CP22n4 — Generated legacy BIOS record handoff (2026-10-03)

A test-only host probe exercises the existing native WinPE root preparation and
shared source/loader patches. The existing generated loader gains an optional
expected drive number; both existing default/second-disk binaries stay exactly
byte-identical. No production source, record provider, helper or Qt route changes
(694 inventoried files unchanged).

Six QEMU/SeaBIOS USB snapshot cases now distinguish the original record behavior:

- NT FAT32 record, 0x81 MBR and stored BPB drive 0x80: `Disk error`.
- NT record with ordinary 0x80 MBR and BOOTMGR-only root: `NTLDR is missing`.
- NT record with fixture BPB drive corrected to 0x81, BOOTMGR only:
  `NTLDR is missing`.
- Same 0x81 fixture plus an NTLDR copy of the prepared BOOTMGR: successful
  generated-loader handoff at 2000:0000, DL=0x81, including its >64-KiB tail.
- Original higher-priority BOOTMGR FAT32 code, fixture BPB drive 0x81:
  the same successful handoff without an NTLDR alias.
- /minint root preparation keeps the loader unchanged; BOOTMGR code and ordinary
  MBR retain the successful DL=0x80 handoff.

This establishes the filename and drive-ID requirements for these generated
components, not a Windows runtime bug or real SETUPLDR compatibility. Preserve
BOOTMGR's original higher selection priority. A Linux FAT32 path must explicitly
handle the BIOS mapping and selected filename, validate any existing NTLDR, and
qualify the complete native workflow before enablement. Do not install aliases
for all legacy images or replace the original NT record merely because a simple
generated loader can run with PE code. Keep the current production gates.

Two fixture details are recorded instead of concealed: tiny emulated USB CHS
translation disagrees with the formatter's 255/63 BPB (nine-GiB sparse disks
resolve it), and the original PE array writes five bytes of FSInfo hints,
advertising zero free clusters to mcopy. PE controls restore known valid hints
without changing executable bytes. Actual filesystem mount/remount metadata
handling remains to qualify; fixture restoration is not a production fix.

All six cases pass normally and with sanitized native probes/leak detection.
The shared 66,020-assertion and native 74-case suites pass both ways. Exact
source/output/file readback, independent original records, precise fsck label
diagnostic and unchanged backing-file fingerprints are checked. All disposable
disks/roots are removed even on failure. Evidence and initial diagnosis logs:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n4`. No user ISO, physical disk
or Windows customization/installation qualification is touched.

**Next: CP22n5 — original NTFS BOOTMGR/0x81 handoff and legacy source contract.**
Prefer qualifying the existing NTFS record with prepared BOOTMGR and the shared
masquerading choice before adding FAT32-specific adaptations. Reuse the existing
source/preflight and one-helper pipeline; make known SIF options, supported layout
and mandatory files explicit before writes. Then complete native creation,
independent readback and BIOS handoff before Qt exposure. FAT32 drive/filename/
FSInfo lifecycle, FAT16, localized no-BOM SIF and real Windows runtime stay in
the backlog. No host inference or additional customization matrix is planned.

## CP22n5 — Original NTFS/WinPE BIOS handoff (2026-10-03)

Rufus's existing NTFS record works unchanged with the prepared generated WinPE
loader and original MBR mapping. Full i386 and amd64 setup layouts reach
2000:0000 with DL=0x81 and the >64-KiB loader tail intact. AMD64 uses its own SIF
while taking the loader/detector from i386, as the shared plan specifies.
/minint keeps the loader unchanged and reaches DL=0x80. No BPB-drive patch,
NTLDR alias, new record provider or production dependency is required for these
BOOTMGR cases.

The original NTFS payload also boots an NTLDR-only control. Its lookup includes
UTF-16LE NTLDR at array offset 520, which the previous ASCII-only audit missed;
the earlier absence observation was incomplete. BOOTMGR is present in ASCII
and UTF-16LE. A control containing neither loader displays BOOTMGR is missing.
This does not qualify an actual Windows/SETUPLDR executable or installed Setup.

Five QEMU USB snapshot cases pass normally and with ASan/UBSan/leak detection.
The 29-case existing NTFS record suite, 66,020 shared checks and 74 native root
cases pass in both builds; all six FAT32 BIOS regressions pass normally after
the common fixture gained an AMD64 layout. Native formatting uses the installed
ntfsprogs-plus mkntfs. A strictly test-only retained NTFS-3G library tool writes
and independently reopens bounded regular-file fixture contents without root,
FUSE or host mounts. Production still uses the existing preferred native mount
driver/NTFS-3G fallback. All 694 production files stay byte-identical.

**First helper source contract — to implement at CP22n6.** Reuse the existing
Windows BIOS/NTFS request and preflight/helper flow, deriving the legacy plan
from freshly scanned source data rather than adding client WinPE-mask/drive
fields or another worker. Before target access/writes require:

- complete retained ISO/UDF analysis, source identity/hash, shared BIOS/NTFS
  eligibility and supported extraction tree; unknown Windows version remains
  unknown rather than being assigned modern installer metadata;
- a complete shared WinPE family, known OsLoadOptions decision and a supported
  GetWinPeSetupPlan action, preserving the original scan/layout priorities;
  unreadable/unsupported options, partial and unsupported minint combinations
  must fail explicitly, not default to false;
- plain MBR/BIOS/NTFS for the first lane: enhanced/UEFI and modern Windows
  customization/silent/regional fields must be refused for legacy sources;
  other legacy formats/features remain tracked, not out of scope;
- selected mandatory detector/loader and any copied SIF as unique readable
  bounded files; valid supported SIF encoding/SetupData insertion and absent
  generated root destinations (case-insensitive), so known refusals occur
  before partitioning; reuse existing ISO readers, extraction checks and
  native/shared transformations rather than another parsing/copy engine.

The worker must recheck extracted inputs/destinations with existing root
preparation. Shared masquerading forces the original Rufus 0x81 MBR when
needed; otherwise retain the original chosen MBR behavior. Reuse NTFS records
and copying, then run root preparation after extraction in the existing private
mount. All errors abort the same authorized job; final sync/unmount/worker exit
is still mandatory before READY. No second authorization or new host detection.
Keep Qt's legacy gate until full helper creation/readback/BIOS handoff passes.

The library fixture initially leaked a cached parent inode; a premature cache
free attempt produced a use-after-free and was removed. Closing the child via
the library's retained-parent operation resolves cleanup; final normal/sanitized
runs pass with no findings or suppressions. Initial failure logs are retained.
Evidence: `/home/marius/proj/lufus/.rufus-test-artifacts/cp22n5`; disposable media
and builds are removed after export. FAT32 BPB/filename/FSInfo lifecycle, FAT16,
localized no-BOM SIF, mixed/existing root loaders, Windows To Go and real
Windows runtime remain in the backlog. No additional customization matrix.

### CP22n6 — retained legacy WinPE source preflight (2026-10-03)

`CheckLegacyWinPeImage` now shares the existing retained-source, ISO/UDF
analysis, extraction-tree, partition-fit and final identity checks. It accepts
only the proposed plain Windows BIOS/MBR/NTFS request without enhanced/UEFI or
modern customization/silent/regional fields. It opens no target and starts no
process or authorization. `CheckIsoWriteImage`, the helper and Qt still reject
legacy creation; this separate read-only entry point is for the next integration.

The legacy checks use shared filesystem/partition policy, complete WinPE masks,
known options and `GetWinPeSetupPlan`. Unsupported or partial minint layouts,
incomplete analysis and mixed boot/install loaders are refused. A bounded
visitor of the existing selected ISO/UDF view finds the mandatory files and
rejects case-ambiguous files/directories or existing generated root names.
Original case is retained for reads. Both the options scanner's SIF and the
root preparation's SIF are checked when their original i386/AMD64 priorities
differ. Existing readers, `InspectWinPeOptions`, `InsertConfigSectionText` and
`PatchWinPeLoader` validate content; no new parser or extraction engine is added.
Unknown Windows version stays unknown and needs no WIM metadata. Prepared root
file sizes are included in the existing partition-fit reservation. This is
bounded source validation, not proof of loader validity or exact allocation fit.

Generated ISO9660/UDF cases cover all three families, mixed i386/AMD64,
case-preserved paths, absent options, i386 `/minint`, the minimum accepted loader,
invalid/oversized/ambiguous inputs, root collisions, SIF refusals, unsupported
settings, cancellation, source changes, capacity, ownership and view cleanup.
The existing modern helper preflight remains a negative control. Normal and
ASan/UBSan/leak runs pass: 490 preflight Qt cases, 74 native root Qt cases and
66,020 shared WinPE checks. GUI/helper builds pass. No original Windows/shared
production source changes. Evidence and reproduction are retained at
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n6`; disposable fixtures/builds
are removed after export. No real USB, Windows ISO or customization test is used.

Next CP22n7: connect the accepted legacy plan to the existing single helper's
NTFS lane. Force original Rufus 0x81 masquerading when the shared plan requires
it; otherwise preserve the selected MBR. After extraction call existing native
root preparation, rechecking inputs/destinations. Qualify a complete generated
ISO/UDF helper job, readback and BIOS handoff before Qt exposure. Keep the
existing sync/unmount/helper-exit completion rule. FAT32/FAT16 lifecycle,
localized SIF, mixed/existing root loaders, To Go and actual Windows runtime
remain required later work; do not add another customization matrix.

### CP22n7 — complete legacy WinPE NTFS helper job (2026-10-03)

The existing Windows BIOS helper now opts into CP22n6's source-derived legacy
checks. Windows source preflight runs before target access/unmounts; the accepted
facts are retained in that worker and source identity is rechecked across later
steps. No client WinPE mask, disk-number field, new worker or authorization is
added. Existing partition/NTFS formatting/PBR, extraction and private-mount
operations are reused. After extraction the existing native root component
prepares detector/SIF/BOOTMGR from the shared plan, then normal sync/unmount/exit
rules apply. Modern version and customization metadata are not invented for
legacy sources. Default GUI preflight/predicate still gates legacy selection.

Full guest creation exposed two real native integration problems:

- Linux's [DOS partition parser](https://github.com/torvalds/linux/blob/master/block/partitions/msdos.c)
  rejects boot indicators other than 0/0x80. Creating 0x81 before kernel refresh
  caused a genuine partition timeout. Keep the normal 0x80 table through
  refresh, formatting and copying; after successful unmount validate that table
  and reuse `CreateMbrImage` to install/read back/fsync the original Rufus 0x81
  table without another rescan. The shared plan forces it even when Win7 was
  selected. Minint keeps the original selected MBR/0x80. This changes the Linux
  operation order, not the finished Rufus record. A replugged 0x81 legacy stick
  has the same ordinary Linux partition-parser limitation as one from Windows
  Rufus; no automatic eject, host workaround or extra prompt is added.
- NTFS-3G's FUSE interface returns EINVAL for `RENAME_NOREPLACE`, confirmed by
  a guest-only syscall trace. Root publication now falls back only for
  unsupported rename flags/syscall to same-directory `linkat` then removal of
  the temporary name. It still refuses an existing destination atomically and
  flushes the directory. Link/publication errors fail the whole job; ordinary
  rename or filesystem errors are not concealed. No plain overwriting rename
  or driver switching is used.

Generated full jobs pass for i386/kernel NTFS (Win7 choice overridden to Rufus
0x81), AMD64/UDF/kernel NTFS under sanitizers, and minint/UDF/NTFS-3G normally
and under sanitizers (Win7/0x80 preserved). Independent readback checks original
MBR/PBR/BPB/backup, clean NTFS metadata, every unchanged source file and exact
prepared root bytes. Bare BIOS snapshots execute the generated 73,728-byte
loader at CS=2000, DL=81 or DL=80 and verify the tail beyond 64 KiB. An injected
first root write failure passes under sanitizers: copy progress reaches its
denominator but the client remains FAILED, cleans/unmounts/reaps/releases the
claim, and does not install final masquerading. One launch per job and delayed
launcher exit prove completion ordering. Authorization is simulated in these
guests; production authorization architecture is unchanged.

One existing modern Windows-7 metadata NTFS helper regression also passes.
Component/preflight suites pass normally and with ASan/UBSan/leaks: 490 Qt
preflight cases, 79 root cases (five new publication cases) and 66,020 shared
setup checks. GUI/helper builds pass. Original Windows/shared production source
and request protocol remain unchanged. Evidence/initial failures/trace are in
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n7`; disposable media/builds
are removed after export. No physical USB, real Windows ISO, actual SETUPLDR or
customization qualification is used. Actual Windows Setup remains unverified.

Next CP22n8: expose the qualified plain legacy BIOS/NTFS choice in Qt using the
shared source facts/plan, preserve Windows ISO/DD selection behavior, and keep
enhanced/UEFI/modern customization choices unavailable for this first lane.
Test ISO load, settings and one GUI job against the qualified helper. FAT16/
FAT32 lifecycle, localized SIF, mixed/existing root loaders and Windows To Go
remain required later work. Do not add another customization matrix.

### CP22n8 — qualified legacy WinPE Qt creation (2026-10-03)

Qt now exposes CP22n7's plain legacy BIOS/MBR/NTFS job. The report predicate
shares the existing WinPE setup plan and preflight's complete-family, known
options, mixed-loader and tree checks; there is no second legacy parser or
client mask. The GUI explicitly opts into the same fresh file/settings checks
at START. Helper code, shared Windows source and request protocol are unchanged
in this checkpoint. No new production file, worker or privilege path.

ISO/UDF i386, AMD64 and minint selection uses Rufus's filesystem preference,
selects MBR with BIOS (or UEFI-CSM), NTFS and the native default cluster, and
uses the image label. Version stays uninferred and modern installation/Windows
User Experience options remain hidden. Actual mandatory-file bounds and root
collisions still fail at START before confirmation/authorization. Partial minint,
mixed loaders and legacy EFI remain unqualified. FAT16/FAT32 remain gated, not
out of scope: the first lifecycle and loader handoff qualified so far is NTFS.

The saved Alt+E preference is retained for subsequent modern images; a qualified
legacy request remains plain and does not silently add dual/UEFI preparation or
customization. Alt+A still supplies the selected ordinary MBR; CP22n7's shared
plan determines any final Rufus masquerading override. The shared ISO/DD policy
and existing recommended-ISO dialog are reused. A valid DD hybrid keeps its DD
choice even when the legacy NTFS/MBR target cannot be offered. Unsupported ISO
settings never veto an explicitly selected DD job.

Normal and ASan/UBSan/leak runs qualify the 16 new scenarios (18 Qt checks
including setup/teardown): family/container/preferences, plain wire request,
no modern dialog, empty/full READY, explicit DD selection, unavailable-MBR DD
policy and short-loader/existing-root failures without authorization. Existing
preflight (490), analysis (46) and the full Widgets regression suite (961 passed)
pass normally and with sanitizers; five pre-existing optional real-image/native-Syslinux
fixture skips are outside this checkpoint, with no legacy skips or suppressions.

A complete generated i386 job also passes normally and with sanitizers through
the actual Widgets/preflight/helper flow in the small disposable guest. It
checks confirmation, one simulated authorization, helper exit held after all
writes/sync/unmount, no premature READY, released exclusive access, final full
READY and unplug behavior. Independent NTFS file/metadata/MBR/PBR readback and
bare BIOS CS=2000/DL=81/large-loader-tail execution pass; the selected Win7 MBR
is overridden by the original shared masquerading plan. No actual Windows Setup
or new customization qualification. The root-only guest driver models client
UID 1000 with a test-only link wrapper and a caller-owned source; production
helper identity validation stays intact. Host component tests run unprivileged.
This does not qualify real host polkit or an actual unprivileged guest desktop.

Initial failures were fixture errors: wrong widget type, an unbootable intermediate
source, omitted raw-controller initialization, an invalid root caller in the
authorization stand-in, a premature asynchronous DD assertion and missing
explicit device reselection. Corrected cases and functional jobs pass; no new
production integration defect was identified. Logs retain those failures.
Selection/READY screenshots were inspected. Evidence and reproduction are saved
at `/home/marius/proj/lufus/.rufus-test-artifacts/cp22n8`; disposable media/builds
are removed after export. The user ISO and dependency sources are preserved.

Next CP22n9: audit the legacy FAT16/FAT32 records and root-loader handoff before
opening more filesystem choices. Resolve the NTLDR/BOOTMGR issue recorded in
CP22n3 using existing providers/shared setup logic, with a small generated BIOS
component check first. Localized SIF, mixed/existing loaders and Windows To Go
remain tracked later work; do not add another customization matrix.

## CP22n9 — Legacy FAT record and metadata audit (2026-10-03)

Completed the audit before broadening Qt's filesystem choices. Production code,
including all original Windows/shared code and the qualified legacy NTFS route,
is unchanged. Extended the existing regular-file BIOS runner rather than adding
another writer, loader, authorization path or guest operating system.

Original WritePBR selects standard DOS code for FAT16, legacy NT code for WinPE
FAT32 and PE code for BOOTMGR FAT32. Shared SetupWinPE creates root BOOTMGR from
the selected SETUPLDR; it does not create NTLDR or the FAT16 record's IO.SYS and
MSDOS.SYS. Original filesystem policy normally offers NTFS for Windows BIOS;
FAT choices require the advanced dual BIOS/UEFI preference. That preference
does not establish that a particular legacy setup supports UEFI.

The new six-case audit copies prepared root files before installing records,
without repairing allocation hints. FAT16 i386 and minint controls reach
Invalid system disk; the i386 control corrects the fixture BIOS drive to 0x81
to isolate the filename/loader problem. Their filesystem checks accept only
Rufus's intentional BPB/root label difference. FAT32's stock PE record with
masquerading stops at Disk error. Correcting only its fixture drive ID permits
prepared i386/AMD64 BOOTMGR to run at CS=2000/DL=81; minint runs at DL=80 without
that correction. All three load the generated 73,728-byte loader's tail beyond
64 KiB. No NTLDR/DOS alias is created; original source files remain unchanged.

Independent fsck also reports a wrong FAT32 free-cluster summary: the original
PE array overwrites five bytes of FSInfo allocation hints. This audit retains
and checks that diagnostic instead of calling boot success clean metadata.
Earlier CP22n4's mount-free PE fixture repaired hints before mcopy; the new
copy-first controls need no repair to boot and expose the remaining metadata
problem directly. This is not a qualification of the existing helper's
mount/remount lifecycle and is not evidence of a Windows runtime defect.

Reuse decision: for the currently supported complete BOOTMGR-only legacy source
contract, reuse Rufus's existing FAT32 PE boot code and shared root setup. A
Linux record adaptation must use the shared plan's 0x80/0x81 drive mapping and
preserve or recompute valid FSInfo allocation hints. This narrowly scoped
departure from original WinPE FAT32 provider selection is justified by the
proven loader-name/drive mismatch, not convenience. Do not add an unconditional
NTLDR alias or a new bootloader, change original NT/PE arrays, or apply this to
mixed/existing-loader sources. FAT16 needs a compatible provider/handoff and
remains required work; the existing DOS record cannot simply be advertised as
support for prepared BOOTMGR.

Validation: all six new BIOS cases pass with normal and ASan/UBSan native
components; the six existing CP22n4 cases still pass normally. Four component
suites pass normally and sanitized: 79 root-preparation cases, 66,020 shared
setup checks, and 115 cases each for existing Windows and WinPE FAT records,
plus their memory-bounds/non-file guards. BIOS failures above are expected
negative controls. No actual SETUPLDR, Windows Setup, new customization test,
physical device, host mount or root authorization is used.

Initial failures were fixture issues: small-disk BIOS CHS translation differed
from the formatter's 255/63 geometry; enlarging before validated MBR creation
correctly produced ESTALE; the new fsck assertion initially expected one extra
blank line. The final sparse nine-GiB FAT16 fixture leaves a 128-MiB partition
and the remainder unpartitioned, avoiding a new partition implementation or
large physical allocation. Logs retain the failed/corrected runs. Evidence and
reproduction: `/home/marius/proj/lufus/.rufus-test-artifacts/cp22n9`. Disposable
builds/media are removed after exporting compact logs/hashes; user ISO and
retained dependencies remain intact.

Next CP22n10: implement/qualify the small FAT32 legacy BOOTMGR record adaptation
on regular files first, keeping existing provider behavior byte-for-byte for
other callers. Check drive mapping, primary/backup records, valid FSInfo,
unaffected reserved gaps, bounds/failure/cancellation/sync and generated BIOS
handoff. Keep helper/Qt FAT exposure gated until a subsequent complete native
job proves its mount/unmount and final-record lifecycle. Preserve NTFS default;
track explicit advanced filesystem selection separately from an unsupported
UEFI boot claim. Localized SIF, mixed/existing loaders, FAT16 and Windows To Go
remain later work. Do not add another Windows customization matrix.

## CP22n10 — Native legacy FAT32 BOOTMGR record adaptation (2026-10-03)

Implemented the regular-file component agreed in CP22n9. Existing
`fat_boot.c/.h` gain `InstallWinPeFatBootRecords`; no production file, bootloader,
writer engine, request field or authorization mechanism is added. It reuses
the original PE record provider and the existing geometry checks, staged
primary/backup/tail records, bounded writes, sync and exact readback. It accepts
the shared setup plan's masquerading decision as a boolean, writes drive 0x81
for masquerading or 0x80 otherwise, and preserves each copy's eight allocation
hint bytes after the original in-memory writer has closed and its unchanged
record matcher has passed. Original arrays and all existing provider behavior
remain unchanged. Preservation adds no FAT scan or per-file allocation work.

This dedicated entry point is FAT32 only. Caller owns a valid private volume
and the supported BOOTMGR-only legacy source contract; it does not validate
source files or recompute/verify hints against FAT allocation. Known primary
hints and an independently unknown backup survive unchanged; both-unknown
hints are also preserved. FAT16 and mixed/existing root-loader sources are
not enabled. Other callers still get original byte-for-byte standard, FreeDOS,
Windows and legacy NT records, including their original allocation-hint and
drive behavior. The targeted Linux departure is justified by CP22n9's proven
BOOTMGR/NTLDR/drive mismatch and overwritten free-count metadata.

Tests extend the existing probe, record suite and BIOS runner. The probe derives
the 0x80/0x81 decision from original GetWinPeSetupPlan for full i386/AMD64/minint
layouts; expected bytes and metadata offsets remain independent of production
constants. CMake registers two adaptation suites (i386 mapping and minint).
Each passes 76 formatting/record/refusal/fault cases normally and sanitized,
including specific primary/backup drive-ID and hint readback corruption,
staging failures, bounds/geometry, reserved gaps, partial writes, cancellation,
sync failure, capacity change, null arguments, FAT16 refusal and descriptor
ownership. Memory bounds and character-device rejection are checked separately.

Existing provider regressions pass normally and under ASan/UBSan with leak
checks: 102 FreeDOS, 102 standard, 115 Windows and 115 legacy NT cases. Root
preparation passes 79 cases and shared setup 66,020 checks. All initial cases
passed; the final normal adaptation rerun adds the targeted metadata-corruption
guards. The helper builds normally with the unchanged public device API; no
new compiler diagnostic is introduced by the adapter/probe.

Four generated BIOS handoffs pass normally and sanitized without fixture drive
correction, hint repair or NTLDR alias: i386/AMD64/minint after file copy, plus
i386 records-before-copy. Prepared root/source hashes, original code with only
the intended metadata changes, MBR/BPB/backup/tails/gaps, independent fsck and
snapshot backing hashes match. Independent fsck reports only Rufus's intentional
BPB/root-label difference, with no wrong free-count summary. The 73,728-byte
generated loader reaches CS=2000, DL=81 for i386/AMD64 or DL=80 for minint, and
checks its tail beyond 64 KiB. This is component boot qualification, not actual
SETUPLDR/Windows Setup or a mounted-device lifecycle test. No new customization
qualification, host block device, mount, root or polkit operation is used.

All 694 previously hashed production files are checked: only the two existing
FAT record files changed; the other 692, including Windows/shared sources,
helper, GUI and protocol, are identical. Evidence, focused baseline/diff, logs,
source/binary/readback hashes, reproduction and cleanup are saved at
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n10`. Disposable builds and
media are removed after export; user ISO and dependency sources remain intact.

Next CP22n11: connect/qualify the legacy FAT32 component on the retained device
descriptor in the existing helper job. Reuse fresh legacy source checks,
formatting, private vfat mount, extraction/root preparation and one-helper
cleanup. Qualify record timing across mount/copy/sync/unmount, primary/backup
allocation hints and final original Rufus/0x81 MBR, without a Linux rescan of
that masquerading flag. Exercise a meaningful failure before success/READY.
Preserve NTFS default; handle explicit advanced FAT selection using Rufus's
filesystem policy without claiming UEFI support for legacy images. Keep Qt
FAT exposure gated until that complete native job passes. FAT16, localized SIF,
mixed/existing loaders and Windows To Go remain later work; do not expand the
Windows customization qualification scope.


## CP22n11 — Complete legacy WinPE FAT32 helper job (2026-10-03)

The existing single-helper creation job now supports the qualified legacy
BOOTMGR-only FAT32 source contract with explicit advanced installation enabled.
NTFS remains the ordinary legacy default and Qt FAT exposure remains gated for
CP22n12. Fresh retained-source preflight uses Rufus's original filesystem and
partition policy with the requested advanced flag, rejects incompatible FAT
content and root NTLDR/IO.SYS/MSDOS.SYS collisions, and preserves the existing
NTFS contract. Version stays unknown; modern customization/UEFI requests are
refused. No request field, shared Windows source or authorization path is added.

The native device entry point calls the existing staged FAT writer. The helper
keeps the normal bootloader phase and validates the original MBR before private
mounting, deferring only the legacy FAT32 PBR writes. After extraction, original
shared WinPE root transforms, successful filesystem sync/unmount and retained
device checks, it writes the adapted PE records with each copy's allocation
hints preserved. The final original Rufus/0x81 MBR is installed and fsynced only
for the shared masquerading plan; minint keeps the selected MBR/0x80. There is
no subsequent partition rescan or remount. Completion still waits for helper
and launcher exit and released access. Existing NTFS and modern FAT32 paths
keep their previous ordering.

Validation: normal and ASan/UBSan host suites pass 518 preflight cases (28 added),
79 root cases, 66,020 shared setup checks, 152 adaptation cases and 115 existing
Windows record cases. Full generated helper jobs pass for i386, AMD64 using the
UDF view, and minint; i386 also passes with sanitized components. Exact file,
MBR/PBR/backup/tail/gap readback, independent fsck and BIOS CS=2000/DL=81 or 80
with the loader tail beyond 64 KiB pass. Primary allocation hints reflect
copied files; the independently retained backup hints are not rewritten merely
to match the primary. fsck accepts only Rufus's intentional BPB/root-label
warning. A root-write failure passes normally and sanitized: EIO/FAILED after
clean unmount and helper exit, with no prepared root files, adapted PBR or
final masquerading MBR. Existing kernel NTFS legacy and modern FAT32 generated
helper/BIOS regressions pass. All jobs use one simulated authorization and no
Windows customization flags; actual Windows Setup and host polkit are unqualified.

The first full job found a real production integration defect: deferred records
also skipped the mandatory bootloader phase, causing the strict client to
reject mounting. Restore that phase with the MBR check and defer only writes;
do not relax the protocol. A subsequent readback failure was a test expectation
mistake (FAT width 32 was compared to a boolean); it is corrected separately.
Focused baseline/diff, logs, hashes and reproduction are saved at
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n11`. Only four existing Linux
production files change; the other 690 checked files, including Qt, protocol,
original arrays and shared Windows sources, are unchanged. Export compact
evidence and remove disposable builds/media; preserve source ISOs/dependencies.

Next CP22n12: expose qualified legacy FAT32 through the existing Qt advanced
filesystem selection, retaining NTFS default and saved Alt+E behavior. Use
Rufus's filesystem policy without displaying or requesting unsupported UEFI
boot for a legacy image. Qualify selection/preflight and one complete Widgets
job, including held-exit READY behavior; reuse existing ISO/DD behavior. FAT16,
localized SIF, mixed/existing loaders and Windows To Go remain required later
work. Do not expand the Windows customization qualification scope.


## CP22n12 — Qt advanced legacy WinPE FAT32 creation (2026-10-03)

Qt exposes the CP22n11-qualified legacy FAT32 helper job through Rufus's existing
Alt+E filesystem policy. Production changes are confined to the existing
selection controller: remove the temporary NTFS-only legacy filter and freeze
the advanced request flag for selected legacy FAT32. Legacy NTFS continues to
request its qualified plain job even when the saved Alt+E preference is enabled.
No helper, protocol, boot record, shared Windows code, callback, authorization
mechanism or new production file is added; 693 other checked files are unchanged.

Ordinary legacy image load still defaults to NTFS. Toggling Alt+E keeps selected
NTFS until the user chooses FAT32; disabling it while FAT32 is selected restores
NTFS. A saved Alt+E preference at new image load uses the original shared Rufus
advanced FAT32 preference, rather than introducing another Linux default rule.
FAT16 stays gated. Pure legacy images retain MBR and BIOS (or UEFI-CSM), BIOS
confirmation text and hidden modern installer options. Version remains unknown;
advanced FAT availability is not a UEFI capability claim. Alt+A's existing MBR
selection and ISO-recommended/explicit-DD dialog remain unchanged.

Final focused Widgets checks pass 32 cases normally and under ASan/UBSan with
leak checks: i386/AMD64/minint across ISO/UDF, ordinary NTFS and advanced FAT32,
frozen filesystem/cluster/MBR flags, no customization dialog, Alt+E refresh and
persistence, FAT16/GPT exclusion, root-loader rejection before authorization,
ordinary/advanced ISO-DD dialog and DD availability when NTFS/MBR is unavailable.
Full normal/sanitized regressions pass five suites: 518 preflight, 46 image
analysis, 76 window, three scaled-layout cases and 974 broader writing/GUI cases.
The latter skips five optional existing fixtures (real Windows/Alpine sources
and three unknown-Syslinux companion successes); no relevant legacy test skips.
The final ISO-DD expansion adds one focused case after the full-suite runs.

The existing Widgets guest creates a generated i386 FAT32 USB normally and
sanitized, choosing the filesystem through the combo and using the same helper.
It checks confirmation, one simulated authorization, correct copy denominator,
held-launcher finalizing below full progress, helper/launcher exit and released
claim before full green READY, then successful unplug. Independent file/root,
MBR/PBR/backup/hint readback, fsck and BIOS CS=2000/DL=81/tail handoff pass. Only
the original BPB/root-label warning is accepted. This uses no real Windows
Setup, new customization qualification, host disk/mount/root or host polkit.
Final guest reruns cover the clearer named filesystem variables in its driver;
the earlier passing jobs and compact manifests remain available too.

No production defect was found in this checkpoint. A build invocation initially
used the wrong executable target name; it was corrected to existing
`rufus-linux` before tests. Evidence, focused diff/baseline, screenshots,
source/binary hashes and reproduction are saved at
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n12`. Export compact evidence
and remove disposable builds/media; preserve user ISO/dependency sources.

Next CP22n13: audit the original legacy dispatch for existing/mixed root-loader
layouts against our current source contract before extending it. Separate
prepared-root collisions, source NTLDR/BOOTMGR semantics and FAT16's DOS handoff;
reuse a compatible existing provider where supported rather than blanket aliases
or relaxed validation. Choose the next bounded implementation from that audit.
Localized no-BOM SIF, remaining FAT16 combinations and Windows To Go remain
required work; actual Windows Setup remains deferred. Keep the one-customization
qualification limit and do not add host locale/code-page guessing.


## CP22n13 — Existing/mixed legacy root-loader audit (2026-10-03)

Audit completed without production changes. Windows's failed no-replace copies
do not imply immediate failure: SetupWinPE retains the existing detector,
edits an existing required root SIF, and patches an existing BOOTMGR in patch
mode. The minint keep path can return success despite an unchecked failed copy.
The Linux refusals therefore cannot all be removed as equivalent cases. An
unused root SIF in /minint mode is already allowed and preserved.

The audit also records Windows's independent MBR/SBR, Syslinux-install/PBR and
post-extraction WinPE priorities, FAT32 BOOTMGR-before-WinPE provider dispatch,
and FAT16's DOS filename contract. Original NTFS disassembly checks BOOTMGR
before NTLDR; string presence alone would not establish execution order.
Mixed modern/EFI/Linux loaders require their own content/dispatch qualification.
Unknown version remains unknown. Detailed source/contract table and provider
constraints are in [the reuse audit](linux-legacy-windows-reuse.md#cp22n13--existing-root-files-and-mixed-loader-dispatch-audit).
Actual Windows file/CRT execution and actual Windows Setup remain deferred.

Add 48 fresh ISO/UDF root-source cases across NTFS/FAT32, i386/minint and equal/
different detector/SIF/loader bytes; add 12 native root-file cases. Current gates,
source/directory/root preservation, unused minint SIF and no outputs after refusal
are tested. Normal/sanitized suites pass 566 source cases, 91 root cases and
66,020 shared checks; no new guest/customization matrix is needed for this audit.
All 694 checked production files are unchanged. Compact logs/source hashes,
provider filename/disassembly evidence, baseline/diff and reproduction are at
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n13`. Export evidence then remove
disposable builds/fixtures; preserve user ISO and dependency sources.

Next CP22n14: admit only an identical existing root NTDETECT.COM through fresh
preflight and the existing native root component, keeping it unchanged rather
than copying it again. Verify bounded regular-file content, case/identity and
source stability before outputs and recheck the retained root input across
preparation. Keep no-overwrite and real error propagation; differing detectors
remain refused until a compatible contract is selected. Qualify meaningful
positive/refusal/race checks and one full existing helper/Qt/readback/BIOS job.
No new copy/record engine, authorization mechanism or callback layer is needed.
Required existing root SIF/BOOTMGR, localized SIF, FAT16 and combined-loader
support remain later work, together with Windows To Go. Keep the existing
one-customization qualification limit and no additional host detection.

## CP22n14 — Retain an identical legacy root detector (2026-10-03)

Implemented the first CP22n13 extension in the existing preflight and native
WinPE component. An existing root NTDETECT.COM is admitted only when its bytes
match the selected loader-directory detector. This follows the original
no-replacement result for that case. Differing detector compatibility remains
unqualified and refused; Linux does not copy over it or ignore an I/O error.

Fresh ISO/UDF preflight retains the selected view's spelling, rejects ambiguous
or non-file root entries, uses the existing bounded reader for both files, and
compares the complete contents. It counts the existing file once in extraction
space; no redundant generated detector is reserved. Source snapshot validation,
unknown Windows version, filesystem/boot dispatch and other root gates remain.
Native preparation uses another entry in its existing SetupFile array for the
root input. It verifies case, regular-file/link/size bounds and identity through
the existing readSource/checkSource functions. It skips detector publication and
rechecks the retained root descriptor/path across SIF/loader publication and at
completion. Its bytes, spelling, inode, mtime and ctime are preserved on success.
No new copy engine, helper protocol, Qt callback, request field or privilege flow.
Only linux/platform/iso_write_check.cpp and winpe_prepare.cpp implementations
and the winpe_prepare.h contract comment changed; 691 other checked production
files, including original/shared Windows code, Qt,
helper and boot records, are unchanged.

Tests update the 48 ISO/UDF/NTFS/FAT32 root cases and 12 native root cases for the
new admission, retaining the unused minint SIF positive control and required
SIF/BOOTMGR refusals. Add 28 fresh-source refusal/error cases and 15 native
retention/error/race cases: same-size differences, empty/oversized/non-file/
ambiguous inputs, root read failure/cancellation, content mutation, identical
replacement, removal, late mutation and directory-detector mutation. Successful
native retention creates only the other files; failures leave no staging files
or descriptor leaks. Normal and ASan/UBSan/LSan suites pass 594 source cases,
106 root cases and 66,020 shared checks. The normal Widgets suite passes 975 cases
with five existing optional fixture skips. Its initial sandbox run could not
start the private D-Bus fixture; an unprivileged rerun with local sockets enabled
passes without production changes. Final reviewed component tests are rerun.

One complete existing Qt/helper FAT32 i386 guest job with mixed-case identical
root NtDetect.Com passes: GUI selection, one simulated authorization, launcher
exit held before READY, exact file/MBR/PBR readback, clean fsck, sync/unmount/exit,
completed READY/unplug handling, and generated BIOS loader drive 0x81/tail marker.
This is generated media, not actual Windows Setup or real-host polkit qualification.
No additional Windows customization or installation matrix is run. Logs, source/
binary hashes, baseline/diff, compact guest evidence and reproduction are at
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n14`. Export evidence, remove the
owned builds/guest disks, and preserve user ISO and dependency sources.

Next CP22n15: handle an existing required root TXTSETUP.SIF using its own content,
as the original SetupWinPE does after the directory copy cannot replace it.
Select/validate that input in fresh preflight; reuse the shared section matcher
and existing encoding/size/error policy. Extend the existing native publication
path only as necessary for an identity-checked atomic edit, preserving unrelated
root content and directory sources. Qualify success/refusal and write/sync/race
failures before another complete existing helper job. Do not substitute the
directory SIF just because it is easier or build a separate patch/copy framework.
Existing root BOOTMGR, localized no-BOM SIF, FAT16, combined-loader layouts and
Windows To Go remain later work. Keep the one-customization testing limit and
no additional host settings detection.

## CP22n15 — Edit an existing legacy root SIF (2026-10-03)

Implemented the selected CP22n14 step in existing ISO preflight/native WinPE
preparation. Original SetupWinPE attempts a no-replace copy and then applies
insert_section_data to the root TXTSETUP.SIF, including an already existing one.
Linux now selects that file's own content when plan.copy_sif is true; it does not
replace it with the directory SIF or demand identical bytes. The shared section
matcher and existing bounded ASCII/UTF-8-BOM/UTF-16LE-BOM adapter provide the edit.
Directory source files and image-derived /minint/loader policy remain unchanged.
Unused minint root SIF remains untouched. BOOTMGR/mixed-provider gates remain.

Fresh ISO/UDF NTFS/FAT32 preflight rejects ambiguous/non-file root entries,
reads the bounded required source and selected root input, validates the root's
own section/text, and reserves output staging space. Native preparation adds
one root-SIF input to the existing SetupFile array, using readSource/checkSource
for no-follow regular-file/link/bounds/case/snapshot validation. The existing
writer stages, reads back, flushes and closes the edited data; preserves root
ownership/permissions without redundant operations on fixed-metadata mounts;
and rechecks all retained sources before an atomic rename to the same root
spelling. After commit it retains the new SIF descriptor/identity and rechecks it
across loader publication and completion. Other new outputs remain no-replace.
No new copy/patch engine, shared Windows change, GUI/request/protocol/authorization
flow, host settings detection or privilege prompt. Two Linux implementation
files and the header contract changed; CMake changes only add test wrappers.

This is the same private-directory, identity-check-before-rename contract used
by the native config writer, not a compare-and-swap operation for an arbitrary
shared directory. The real helper owns its exclusive private mount. A failure
before replacement preserves the original SIF; a later sync/loader failure can
leave the complete edit committed and must fail the job. The set of root files
is not one transaction. Temporary output/descriptor cleanup is still mandatory,
and full READY still waits for device/filesystem sync, unmount and helper exit.

Review found and tests reproduced a real space-planning error in the first
implementation: counting only the root SIF's growth admitted a synthetic
near-capacity partition although atomic editing needs the whole edited file while the original
still exists. Four ISO/UDF NTFS/FAT32 regressions fail with admission success
before the fix. Reserve the full staged edit, alongside other prepared outputs;
all four now reject ENOSPC before target access and admit a control with enough
stage space. These model logical tree sizes through the existing test wrapper;
they do not format a physically full disk or add a filesystem-overhead estimator.
Existing identical detectors still need no redundant output space.
An earlier new test used an invalid ANSI byte but expected the valid non-ASCII
no-BOM error; split valid unsupported no-BOM (ENOTSUP) and invalid UTF-8 (EILSEQ).
No production encoding policy changed to make the test pass.

Normal and ASan/UBSan/LSan checks pass 662 source cases, 143 native root cases and
66,020 original/shared setup checks. New cases cover distinct root content,
repeated sections, ASCII and Unicode BOMs, i386/AMD64/mixed directory selection,
a directory SIF without the root edit section, staging capacity, bad text/size/
kind/case/link inputs, root-specific read/cancel, write/verify/sync/close/mode/
rename failures, root replacement/removal/ambiguity/content races and mutation
of the committed SIF during loader output. Unrelated root content and directory
sources survive; root spelling/ownership/permissions and failure cleanup are
checked. The focused existing Widgets matrix passes 34 cases, including two new
invalid-root-SIF refusals before confirmation/authorization; final preauthorization
cases are rechecked after the space fix. A complete existing Widgets/helper FAT32
i386 job with distinct mixed-case root SIF and identical root detector passes
exact seven-file/MBR/PBR readback, fsck, one simulated authorization, held launcher
exit before READY, sync/unmount/exit, unplug handling and generated BIOS drive
0x81/tail marker. Rerun that same job after the production space fix also passes.
No actual Windows Setup/host polkit qualification or new customization matrix.

Evidence/baseline/diff/reproduction and compact guest results are at
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n15`. Export evidence then remove
owned builds, generated ISO/tree/initramfs/disks; preserve user ISO and dependencies.

Next CP22n16: audit existing root BOOTMGR backed by a legacy SETUPLDR, beginning
with an identical directory-loader/root-file candidate. Resolve the original
name-based report and FAT32 NT-versus-PE provider priority before changing source
admission or Qt selection; do not blindly clear HAS_BOOTMGR or treat a modern
manager as a legacy loader. Reuse this native publication/validation path once
that source/record contract is established. Localized no-BOM SIF, differing
detectors, FAT16, combined loaders and Windows To Go remain later work. Keep
Windows runtime validation deferred and the single-customization testing limit.


## CP22n16 — Existing root BOOTMGR source/record contract audit (2026-10-04)

Audited the next root-loader case without changing production behavior. Original
CheckIsoScanFile marks root BOOTMGR by case-insensitive filename, independently
of bytes or complete legacy setup files. CheckIsoEfiFile separately marks root
BOOTMGR.EFI as EFI; it is not evidence of a usable legacy BIOS loader. A renamed
SETUPLDR therefore keeps HAS_BOOTMGR_BIOS, complete WINPE bits, readable options
and an unknown Windows version. Never clear that report flag to pass a pure
legacy predicate: original WritePBR selects FAT32 PE records for HAS_BOOTMGR
before its legacy FAT32 NT branch for HAS_WINPE. The retained flag selects the
BOOTMGR provider, already used by the qualified native FAT32 adaptation.

Original SetupWinPE still runs after BIOS extraction when HAS_WINPE is true,
regardless of that root filename flag. Its no-replace loader copy fails on an
existing root file. Patch mode opens and transforms that existing root file;
keep-loader /minint leaves it alone. GetWinPeSetupPlan still controls directory
selection, the SIF copy and 0x81 masquerading, independently of HAS_BOOTMGR.
The original NTFS common record searches BOOTMGR before NTLDR, as its disassembly
at offsets 0x3ae..0x3d6 confirms. FAT32 PE contains BOOTMGR at 0x169; FAT32 NT
contains NTLDR at 0x170. Provider binaries, original header hashes and disassembly
are saved. This is original source/static provider analysis, not new Win32
runtime, mixed-loader BIOS or actual Windows Setup qualification.

Select the first extension narrowly: one bounded regular root BOOTMGR identical
to the selected directory SETUPLDR before any transformation, with the already
supported complete plain-BIOS layout. Equality means the existing root supplies
the same input that Rufus would copy. It does not authenticate that input as a
Windows executable: the existing shared patcher tolerates absent patch markers.
The directory-source contract and generated handoff limits still apply. Refuse
one-byte differences, separately prepatched files and unrelated/modern managers
until their own compatible input contract is established. Do not use patch
markers or an invented Windows version as a substitute for full equality.
Keep root BOOTMGR.EFI, modern installation archives and other loader combinations
outside this extension; FAT16 and source-native NTLDR remain separate providers.

For native patch mode, read/check both original inputs before outputs, transform
the retained root input and reuse the existing SIF atomic replacement path.
Preserve root spelling/ownership/permissions; validate the newly committed
loader descriptor/path through completion. Reserve its whole staged output in
future ISO preflight, alongside the original. For keep-loader minint, retain
and recheck the root without creating a staging copy or modifying metadata.
The existing private exclusive mount/identity-before-rename contract remains;
this is not atomic pathname compare-and-swap or a root-set transaction. A late
error must fail the job even if a complete prior output was committed.

New generated-source regression cases exercise i386, AMD64 and minint in ISO/UDF
views with absent, identical, one-byte-different, already-patched, opaque and EFI
root inputs (the minint keep plan has no patched case). All 34 rows check both
plain NTFS and advanced FAT32. They verify the unchanged name-derived BIOS/EFI
flags, complete setup/options, unknown version, unchanged masquerading plan,
current refusal of every existing root loader before target access, descriptor
cleanup and unchanged source/directory/root bytes. Absent-root controls retain
both supported routes. This checkpoint intentionally changes only that test
and documentation; all 694 production-file hashes match CP22n15. No new GUI
availability or source admission is implied by these refusal checks.

Normal and ASan/UBSan/LSan suites pass 696 source cases and 66,020 shared setup
checks with no failures/skips. The focused new source rows pass 36 QtTest cases
including init/cleanup. No GUI/VM job is rerun because its production path and
available source contract are unchanged. No physical USB, root authorization,
Windows customization or installation matrix. Baseline/diff, hashes, logs,
provider audit and reproduction are at
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n16`.
Export evidence and remove disposable builds; preserve user ISO and dependencies.

Next CP22n17: implement identical existing root BOOTMGR in the native root
component only, reusing SetupFile/readSource/checkSource and existing atomic
replacement/retention. Cover patch versus keep, spelling/metadata, identical
and changed inputs, staging/sync/publication errors and committed-input races.
Keep source/Qt gates in place for this component checkpoint. Then CP22n18 adds
fresh bounded ISO/UDF equality checks and candidate routing with HAS_BOOTMGR
preserved, followed by focused Qt refusal/selection and one existing full
helper/readback/BIOS job. No parallel copy/patch framework or new authorization.
Localized no-BOM SIF, differing detectors, FAT16, combined loaders and Windows
To Go remain required later work. Real Windows validation stays deferred and
the one-customization qualification limit remains.


## CP22n17 — Native identical existing root BOOTMGR (2026-10-04)

Implemented the CP22n16 native-only checkpoint. PrepareWinPeRootFiles now reads
an existing root BOOTMGR into another entry of its SetupFile array, through the
same bounded regular-file/link/case/path/identity checks as the other inputs.
Require full equality with the selected directory SETUPLDR before applying any
shared patches. Then select the root bytes themselves as the loader input.
One-byte differences, prepatched roots and opaque different managers remain
refused. Equality preserves the existing copied-input contract; it does not
prove a Windows executable. Directory sources remain untouched.

Patch mode uses the existing SIF storeFile replacement path, without modifying
its write/readback/sync/metadata/publication logic. It preserves root spelling,
owner/group and permission bits; writes and checks a complete temporary output;
rechecks the retained sources; atomically renames over the verified root name;
and retains/rechecks the new loader descriptor/identity through completion.
Keep-loader minint skips publication and keeps validating the original root
through other outputs and completion. This distinction follows plan.action,
not uses_minint alone: an i386 layout with /minint can still use patch mode.
If all minint root files already exist compatibly, this component performs no
writes; the complete helper still owns final filesystem/device sync and cleanup.

All existing private-directory guarantees remain. A prepublication failure
preserves the loader unless an injected/external change itself modified it;
earlier complete detector/SIF outputs may remain. A following directory-sync
failure leaves the complete loader edit committed and fails the job. This is
identity-check-before-rename on the caller's exclusive private root, not atomic
pathname compare-and-swap or a transaction for every root file. No new writer,
patcher, callback/protocol/request field, privileges, host detection or original
Windows change. Only winpe_prepare.cpp implementation and its header contract
change; the other 692 checked production files, including shared Windows code,
preflight, Qt, helper and boot records, are unchanged. No CMake wrapper changes.

Tests update the existing 12 root-file cases for identical loader admission and
add eight success rows: i386, AMD64, mixed, keep-minint, patch-i386 with /minint,
multiple transfer buffers, all existing patch roots and all retained minint
roots. They verify exact transformed/retained bytes, spelling, owner/group/mode,
replacement versus retained inode/times, source-directory preservation, combined
SIF/detector behavior, no loader staging in keep mode, descriptor/staging cleanup.
The eight success rows fail with EEXIST against the saved pre-change native
component, demonstrating the missing behavior independently of the new code.

Thirty-four new refusal/fault rows cover differing/prepatched/opaque, empty/
oversized, ambiguous, linked and special root files; root-specific read/cancel;
loader write, verify, sync, close, chmod, rename and cancellation errors; content
and identical-path replacement before or during loader output; removal and case
ambiguity; directory-source mutation; postcommit directory sync and mutation of
the committed loader. Six cases also check retained minint inputs. The existing
symlink-destination test expected the old unconditional EEXIST collision. The
new root read reaches O_NOFOLLOW and correctly returns ELOOP; update that test
expectation without weakening validation. Initial logs retain that one stale
expectation failure. A baseline comparison command also needed its Ninja shell
wrapper removed and shared C object archive retained; these were evidence-tool
errors, not production/fixture failures. Final normal and ASan/UBSan/LSan runs
pass 185 native cases, 696 source cases and 66,020 shared setup checks with no
failures/skips. Source tests still prove existing-root loader admission is gated.

No GUI/VM route is newly exposed or qualified here, and no physical USB, root
authorization, actual Windows Setup or additional customization test. Baseline,
checkpoint diff, before/after production and binary hashes, initial/final tests
and reproduction are at
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n17`.
Export evidence then remove disposable builds; preserve user ISO and dependencies.

Next CP22n18: add fresh bounded ISO/UDF root-loader equality/size/read/error checks
and reserve the full patched loader stage alongside the extracted original;
keep-minint reserves no duplicate output. Route the complete plain-BIOS legacy
candidate with HAS_BOOTMGR_BIOS preserved, while retaining EFI/archive/other-loader
gates and unknown version. Validate differences before authorization. Match
Qt selection to that candidate/preflight contract and run focused GUI checks and
one complete existing helper/readback/BIOS job before exposing the route. Reuse
the existing original PE/NTFS providers and job/authorization/completion flow.
Localized no-BOM SIF, differing detectors, FAT16, combined loaders and Windows
To Go remain later requirements. Real Windows validation remains deferred and
the single-customization qualification limit still applies.


## CP22n18 — Existing root loader source admission and Qt integration (2026-10-04)

Enabled the CP22n16/17 identical-root BOOTMGR contract through fresh source
preflight and the existing Qt/helper job. The legacy predicate now admits a
complete plain-BIOS candidate with HAS_BOOTMGR_BIOS preserved; EFI BOOTMGR,
modern archives and other loader families retain their gates. This predicate
establishes a candidate, not byte compatibility. Preflight walks the retained
ISO/UDF view, rejects duplicate/non-file root names, reads both bounded loader
inputs, requires full equality before patches, then transforms the root bytes
through the shared loader patcher. One-byte changes, separately prepatched roots
and unrelated managers remain refused before target access/authorization.

Use the existing reader/walker/prepared-output accounting. Patch mode reserves
the entire staged root loader alongside its extracted original; keep-minint
reserves no redundant loader output. Existing SIF staging and detector retention
remain. The header now describes both existing root inputs and full edit-stage
space, rather than the outdated absent-root/plain-NTFS-only contract. No version
is inferred or filename flag cleared; equality preserves the copied input and
does not authenticate a Windows executable. Shared original boot providers,
native root writer, helper/request/protocol/authorization and completion flow
remain unchanged.

The first Widgets run exposed a real integration gap: root BOOTMGR marks
Windows-version analysis pending even with complete readable legacy setup files
and no modern archive. The candidate could select BIOS, but isoWriteChoices
excluded ISO_PENDING_WINDOWS, leaving the partition chooser empty and START
unavailable. Allow that pending bit only when the same legacy predicate accepts
the complete source report. Keep the pending bit, unknown version and Partial
analysis state intact; do not claim complete Windows metadata. Fresh START checks
remain mandatory, including for invalid-root candidates. Unknown modern, mixed,
EFI or incomplete sources do not receive this exception. The fixture's Complete
expectation was also updated to recognize the legitimately retained Partial
state; that expectation alone did not fix the real empty-choice bug. Initial
Widgets failure logs are retained, with final passing runs.

Source tests update the prior root/report cases for candidate versus successful
admission, preserve BIOS/EFI flags and check prepared-byte budgets. Add 36 ISO/UDF
NTFS/FAT32 refusal/error cases for empty/oversized/non-file/ambiguous roots,
root-specific read/cancel and EFI/archive/Syslinux companions. Eight staging
cases require full patch-stage space or demonstrate minint retention without a
stage; they also include enough-space controls. These simulate logical tree
sizes, not physically full media or new filesystem-allocation overhead estimates.
Normal and ASan/UBSan/LSan checks pass 740 source cases, 185 native cases and
66,020 shared setup checks, with no failures/skips. Native CP22n17 logic is
unchanged. Only preflight implementation/header and three lines of Qt selection
logic change; all other 691 checked production files are unchanged.

The focused Widgets matrix passes 56 cases. Eighteen new matching-root selection
rows cover i386/AMD64/minint, ISO/UDF, NTFS/advanced FAT32 and original MBR choices;
verify preserved BOOTMGR and pending-version flags, unknown version, BIOS/MBR
selection, filesystem/default clusters, absent modern customization, confirmation,
one simulated helper start and completed READY. Four additional bad-root cases
stop before confirmation/authorization, preserve the target and never report
successful READY. Existing hybrid ISO/DD, advanced refresh, excluded FAT root
loaders and oversized-target DD controls continue passing. The complete existing
Widgets regression suite passes 999 cases with five existing optional-fixture
skips (real Windows/Alpine selection and upstream unknown-version Syslinux companions).

One complete generated i386/FAT32 Qt/helper job contains matching mixed-case root
BoOtMgR and NtDetect.Com plus a distinct root SIF. The existing guest recipe gains
only a root-loader input flag and expected filename/hash; no parallel job/boot
engine. Exact seven-file readback proves the root loader and root SIF edits and
unchanged directory sources/detector. Original MBR/PE PBR, per-copy FSInfo hints,
clean fsck, one simulated authorization, held launcher exit before full READY,
sync/unmount/helper+launcher exit and unplug handling pass. Generated BIOS reaches
`RUFUS_WINDOWS_BOOTMGR_OK cs=2000 drive=81 tail=ok`; snapshot remains unchanged.
This is generated compatibility/record/lifecycle qualification, not actual
Windows Setup, real-host polkit or a new customization/installation matrix.

Evidence, baseline/diff, production/binary hashes, initial/final tests, compact
guest results and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n18`.
Export evidence then remove temporary builds and generated media; preserve user
ISO and dependency sources.

Next CP22n19: audit localized no-BOM SIF handling against the original parser and
our existing text adapter. Establish how the original chooses encoding and what
reliable source-owned facts or explicit user choice can supply the Linux input
contract before broadening admission. Do not infer it from host locale/settings,
force UTF-8 onto ambiguous legacy bytes or add a second parser/writer. Keep
encoding/size/error preservation and useful focused tests. Differing detectors,
nonidentical/prepatched root loaders, FAT16, combined dispatch and Windows To Go
remain required later work. Real Windows validation and the single-customization
qualification limit remain unchanged.


## CP22n19 — Localized SIF encoding contract audit (2026-10-04)

This is an audit and focused test checkpoint, with no production change. All 694
production hashes match CP22n18. Localized no-BOM SIF editing remains unresolved;
it is not enabled by treating every legacy file as UTF-8 or by host detection.

Original `parser.c:insert_section_data` opens input with `_wfopen(...,
L"r, ccs=UNICODE")`, reads a two-byte `bom` through that stream, selects plain,
UTF-8 or UTF-16LE output from that value, seeks to the beginning, and uses
`fgetws`/the shared `IsConfigSection`/wide output. It neither reads an image NLS
setting nor accepts a code-page argument. The plain-output branch and its ANSI
comment alone do not establish how a given Windows CRT decodes the input.
BOM handling has already happened at open; the following `fread` is not a raw
file-header read. Its observed value and the actual output round trip still need
real Windows qualification. Do not describe this static audit as a confirmed
Windows parser bug or CRT parity test.

The primary CRT documentation is inconsistent here:
[the fopen table](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fopen-wfopen?view=msvc-170)
specifies UTF-16LE for `ccs=UNICODE` with no BOM and maps that mode to `_O_WTEXT`.
[The low-level open documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/open-wopen?view=msvc-170)
says reading `_O_WTEXT` without a BOM falls back to ANSI. These descriptions
cannot resolve the deployed Windows runtime's behavior on their own. Keep that
execution check deferred as requested rather than copying an uncertain CRT quirk
into the native adapter.

The existing Linux section adapter explicitly accepts ASCII/no-BOM, UTF-8/BOM
and UTF-16LE/BOM, retaining BOM, CRLF and Rufus's 1023 UTF-16-unit chunks. Valid
non-ASCII UTF-8 without a BOM is still ambiguous: bytes C3 A9 are U+00E9 in UTF-8
but U+00C3 U+00A9 in Windows-1252. A bare E9 is valid Windows-1252 but invalid
UTF-8; 82 A0 is a Windows-932 character but invalid UTF-8. Existing ENOTSUP versus
EILSEQ results describe this adapter's admission/decoder, not a verdict that a
legacy file is corrupt. A generic ISO token edit already has an explicit UTF-8
contract; this audit does not change that separate contract.

An `[NLS]`/`AnsiCodepage=c_1252.nls` hint or bundled NLS filename is not consumed
by Rufus's original insertion routine and does not declare the SIF byte encoding.
[ReactOS's source SIF](https://github.com/reactos/reactos/blob/master/boot/bootdata/txtsetup.sif)
provides a primary example of the NLS filename syntax; it is not Windows runtime
qualification or a newly supported ReactOS image. Here that syntax is synthetic
fixture content. A BOM-marked Unicode SIF can contain such a setting and Unicode
characters outside the named ANSI page. Preserve them rather than transcoding to
that page. Any future no-BOM extension must supply an explicit, frozen input
encoding (or a documented, reliable format declaration) to the same adapter and
use an existing codec, preserving unrelated bytes and output limits. No codec
framework, new chooser, request field or second parser is added by this audit.

Rufus's shared `plan.copy_sif == false` is a different case: `/minint` preparation
does not edit/copy the SIF at all. Existing ASCII-syntax options inspection can
read `/minint` around localized value/comment bytes. Do not impose the text-edit
encoding gate on that retained path. Tests now exercise Windows-932 directory
SIF bytes and distinct Windows-1252 root SIF bytes across the existing ISO/UDF,
NTFS/FAT32 root matrix; native checks prove byte/inode/mtime retention. Missing
SIF in the direct native component remains covered; source preflight still
requires readable options to establish the plan.

Added twelve source refusal rows and five native rows, reusing existing test
fixtures and matrices. Refusals preserve source/root bytes and create no target;
BOM-positive cases include localized text and a conflicting NLS hint. Complete
focused suites pass normally and with ASan/UBSan/LeakSanitizer: 752 source cases,
190 native cases, 43 generic config cases and 171 options checks. The 51-case
focused section/native subset also passes under both C and Romanian UTF-8 test
locales. No host-device, GUI/full-helper rerun or extra Windows customization/
installation qualification is needed for this test/documentation-only change.

Evidence, baseline/diff, source/binary/production hashes, commands and logs:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n19`. Export compact evidence,
verify it, then remove both temporary builds. Preserve the user's ISO and deps.

Next CP22n20: extend the existing retained-root NTDETECT.COM path to different
bounded regular content, following the original no-replace-copy behavior already
established in CP22n13. Reuse the same reader and identity/cancellation/final
checks; retain the selected directory detector as an unchanged source and never
rewrite the existing root detector. Remove only the detector equality gate;
BOOTMGR equality and all file-kind/link/case/size/error gates remain. Qualify the
native/source behavior in a small checkpoint before any further boot-chain
extension. Localized no-BOM editing still needs the explicit encoding contract;
FAT16 needs a compatible handoff (the existing original DOS record audit is
already complete), mixed dispatch and Windows To Go remain required work.


## CP22n20 — Retain a differing root NTDETECT.COM (2026-10-04)

The original `src/wue.c:SetupWinPE` calls `CopyFileA(src, dst, TRUE)` for
NTDETECT.COM and continues without replacing an existing root detector. Unlike
its root SIF and loader operations, it neither transforms that detector nor
compares it with the directory source. Linux now follows that existing-file
behavior for differing contents too. Retention does not authenticate the file
or establish that a particular detector will execute under Windows Setup.

Production removes two native equality-check lines and narrows the existing
source equality condition to BOOTMGR only. The two existing API headers document
the contract; all other 690 production hashes, including original/shared Windows
code, Qt, helper/protocol, authorization and completion flow, are unchanged. No
new reader, writer, adapter layer, request field, callback or payload engine.

The native component reads the root detector through its existing SetupFile
entry, retains its descriptor and spelling, and performs the existing identity/
size/mtime/ctime/cancellation rechecks during publication and at completion.
It never stages, overwrites, patches or changes metadata on the retained file.
The selected directory detector is still independently required/validated and
left unchanged. Unique nonempty regular inputs remain bounded to 8 MiB; links,
special entries, case ambiguity, unreadable/changed/replaced/removed sources,
cancellation and I/O errors still fail. Newly appearing output collisions also
still fail rather than silently selecting a late unvalidated input. This keeps
our established private-directory/source-preservation/error contract.

Fresh ISO/UDF preflight uses the same bounded root read, removing only detector
content equality. Existing loader equality, root SIF encoding/own-content rules,
report flags, unknown Windows version, filesystem/layout and mixed-loader gates
remain. Retained detectors require no generated duplicate/staging reservation;
both equal-sized different data and a multiple-buffer binary input assert the
exact prepared SIF/loader reservation across NTFS/FAT32 and ISO/UDF.

Tests reuse existing root/fault matrices. The native matrix now covers equal and
different root bytes through read/cancel, invalid kinds/bounds/links/case, early
and late mutation, identical replacement, ambiguity/removal and directory-source
change. Its replacement fixture now actually preserves the replaced file's own
bytes for both input variants. Success checks preserve bytes, inode, ownership,
mode and timestamps, require only two writes (SIF and loader), and verify the
unchanged directory detector/loader. A binary detector spanning multiple read
buffers is retained too. Existing i386/minint root matrices cover both contents.

Before the production change, the focused native/source tests fail 9/16 cases
with the old EEXIST gate; logs are saved. The first complete post-change source
run found two older generic refusal fixtures still expecting differing detectors
to fail. Replace those with genuinely invalid empty root detectors; positive
retention is independently covered above. That was an obsolete expectation,
not a new production failure. No other production change was needed.

Final normal and ASan/UBSan/LeakSanitizer component suites pass: 756 source cases,
206 native cases and 66,020 shared setup checks, no failures/skips. Focused Qt
Widgets passes 58 cases. The existing 18 matching-root-loader positive GUI rows
now also contain a differing mixed-case root detector and check it remains
unchanged before confirmation; shared request/defaults/unknown version, one
simulated authorization, helper completion and READY assertions remain. Empty,
directory and duplicate root detectors fail before authorization. GUI production
code is unchanged; this is focused preflight/request/lifecycle integration with
the existing simulated helper, not a new full guest or actual Windows Setup
qualification. Existing CP22n14/18 complete helper jobs remain the baseline.
No additional Windows customization or installation matrix was run.

Evidence, 12-file baseline/diff, production/source/binary hashes, initial/final
logs and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n20`.
Export and verify evidence then remove both temporary builds, preserving source
ISO and dependencies.

Next CP22n21: use the existing complete Qt/helper i386 FAT32 guest recipe with a
differing root detector, matching root BOOTMGR and distinct editable root SIF.
Add only a fixture-content choice to the existing recipe; independent readback
must show the retained detector and unchanged directory sources plus the expected
root edits. Reuse the existing record/BIOS and one-authorization/READY checks;
do not add another job engine or Windows customization test. Afterwards address
nonidentical/prepatched root-loader admission using the original shared patcher
and clear source contracts. Localized no-BOM encoding, compatible FAT16 handoff,
mixed dispatch and Windows To Go remain required work.


## CP22n21 — Complete differing-detector Qt/helper readback (2026-10-04)

CP22n20's native/source behavior is now qualified through the existing full
Qt/helper i386/FAT32 guest recipe, with differing root NTDETECT.COM, matching
root BOOTMGR and a distinct editable root TXTSETUP.SIF. This changes only fixture
selection/readback metadata and documentation. All 694 production hashes match
CP22n20, including the native preparation, Qt, shared Windows code, helper/
protocol and boot providers. No new job engine, callback or authorization path.

The original bare root-detector flag remains boolean and generates identical
bytes by default. A separate test-only `--windows-winpe-root-detector-content`
choice selects `identical` or `different`; the latter requires that root flag.
The differing generated root is 131,110 binary bytes, spanning multiple read
buffers, versus the unchanged 28-byte directory detector. Source/target/job and
readback manifests record the choice without mislabeling different bytes as
identical. Existing root-loader presence is also recorded in final readback.
Two compact fixture controls verify the unchanged identical default and differing
content, exact seven-file manifests and retained hashes. Two CLI dependency
refusals pass before any output creation. The first CLI evidence command omitted
the already-required kernel option, so it stopped at argparse rather than the
new dependency check; the corrected command passes, and both logs are retained.
No production fault or workaround was involved.

One existing complete Widgets guest job passes with the production helper.
Independent readback proves the different mixed-case root detector and the
selected directory detector retain their own source hashes. The root SIF has
its expected SetupSourceDevice insertion and preserves root-only content; the
root loader has the shared expected patches. All three directory sources and
data/copy.bin remain unchanged, and exactly seven files are present. The copied
byte denominator includes the larger retained root input. The selected Win7 MBR
is correctly superseded by the original shared WinPE masquerading policy; the
original Rufus MBR/PE PBR, geometry, backup boot sector and independent FSInfo
hints are checked. Nonwriting fsck permits only its precisely matched existing
original-boot-label warning (NO NAME in the original PBR versus RUFUS_TEST in the
root); there are no other filesystem diagnostics. Do not describe this as a
zero-warning fsck or repair the original records merely to hide that notice.

The guest holds the launcher after the helper finishes and asserts Finalizing
with progress below full READY, then releases it and requires clean sync/unmount/
helper+launcher exit, released claim and full green READY. Exactly one simulated
authorization is recorded. Unplugging after success preserves READY. Generated
BIOS reaches `RUFUS_WINDOWS_BOOTMGR_OK cs=2000 drive=81 tail=ok`, and snapshot media
is unchanged. This exercises record/loader handoff and the complete Linux job;
it does not execute the synthetic detector, prove Windows Setup compatibility,
exercise real-host polkit or add a Windows customization/installation test.

CP22n20's component normal/sanitizer and focused Qt evidence remains applicable
because production is byte-identical; it was not needlessly repeated here.
Build, fixture/CLI/syntax controls and complete guest job pass. Final helper and
GUI binary hashes match the actual guest input manifest. Evidence, eight-file
baseline/diff, source/production/binary hashes, compact guest logs/manifests and
reproduction: `/home/marius/proj/lufus/.rufus-test-artifacts/cp22n21`.
Generated large media is already removed by the recipe; verify exported evidence,
then remove the temporary build and remaining guest directory. Preserve user ISO
and dependency sources.

Next CP22n22: address prepatched/nonidentical existing root BOOTMGR inputs using
the original shared loader transformation and provider policy. Concentrate on
which transformed inputs preserve the selected legacy setup contract; the
original root-file/provider-priority audit is already done and need not be
repeated. Keep version unknown, avoid host inference or a second patcher, and
retain bounded file/identity/error checks and clear admission limits. Localized
no-BOM SIF encoding, a compatible FAT16 handoff, mixed-loader dispatch and Windows
To Go remain required work. Actual Windows validation and the one-customization
qualification limit remain unchanged.


## CP22n22 — Existing loader admission through one shared patch pass (2026-10-04)

Native preparation and fresh ISO/UDF preflight now accept an existing root
BOOTMGR when its result after one shared PatchWinPeLoader pass equals the
selected directory SETUPLDR result after one pass. This admits ordinary fully
or partially prepatched inputs. Keep-loader minint still requires unmodified
equality and retains the root without writing it. The original Windows/shared
patcher, report flags, provider priorities and unknown version are unchanged.
Only the two Linux adapters and their contract comments change in production;
all other 690 production hashes match CP22n21. No new parser, job engine, host
inference, authorization path or loader authenticity claim.

The one-pass contract matters: the original scan advancement can skip a second
adjacent path marker on the first pass and patch it on the next. New shared tests
compare both passes with the original reference and demonstrate that repeated
patching is not universally idempotent. Do not normalize until stable or blindly
accept any prepatched input. Wrong-family, wrong-drive, altered-tail, opaque and
nonconverging second-pass inputs remain refused before publication/authorization.
These other root loaders remain required parity work, not removed from scope.

The root itself receives exactly one original transformation and is published
through the existing verified, flushed, identity-checked atomic writer. File
bounds/kind/link/case checks, retained descriptor/path/mutation checks, metadata
and directory-source preservation remain. Full loader staging space is reserved
even when a prepatched root already equals its final output; keeping minint needs
no loader stage. Multiple-buffer and existing SIF/detector combinations are covered.
Prepatched roots also exercise the existing read/cancel/write/verify/sync/rename/
close/mode and mutation/replacement/removal faults. Replacement controls now copy
the actual root bytes so inode replacement is independently detected even when
content is identical.

Before the production change, focused tests expose 27 native and 8 source
failures at the old EEXIST gate. Final normal and ASan/UBSan/LeakSanitizer component
runs pass: 790 source cases, 240 native cases and 66,040 shared checks, no failures
or skips. Focused Widgets passes 70 cases, including 12 new prepatched i386/AMD64
ISO/UDF NTFS/advanced-FAT32 rows. Source bytes remain unchanged before confirmation;
unknown Windows version, request/defaults, one simulated authorization and full
READY assertions remain. This GUI run uses the existing simulated helper; native
real-file preparation is covered separately. No new complete guest, Windows Setup,
host polkit or customization qualification is claimed.

Evidence, 13-file baseline/diff, production/source/binary hashes and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n22`.
Verify exported evidence and remove both temporary builds; preserve source ISO
and dependency sources.

Next CP22n23: reuse the existing complete i386 FAT32 Qt/helper guest recipe with
a prepatched root loader, differing retained detector and distinct editable root
SIF. Add only an explicit fixture-content choice, preserving its identical-loader
default; independent readback must prove final root bytes and unchanged directory
sources. Reuse record/BIOS, one-authorization and durable READY/unplug checks.
Afterwards resume the remaining legacy/mixed-layout and Windows To Go backlog;
localized no-BOM SIF still needs an explicit encoding contract, and FAT16 still
needs a compatible original-provider loader handoff. Actual Windows validation
and the one-customization qualification limit remain unchanged.


## CP22n23 — Complete prepatched-loader Qt/helper readback (2026-10-04)

CP22n22's loader admission now passes the existing complete i386/FAT32 Qt/helper
guest recipe with a prepatched root BOOTMGR, differing retained NTDETECT.COM and
distinct editable root TXTSETUP.SIF. All 694 production hashes match CP22n22.
This changes only the fixture content choice/readback metadata and documentation;
there is no new runtime patcher, request field or job engine.

The original root-loader flag stays boolean, with identical content by default.
A separate `--windows-winpe-root-loader-content prepatched` choice uses the
fixture's existing independently expected loader bytes as its root input. It
requires that flag and an i386/AMD64 patch-loader fixture; keep-minint is refused
before output creation. Four small fixture/hash controls cover identical and
prepatched i386, prepatched AMD64 and the unchanged identical minint default.
They check seven source/target files, root/directory hash distinctions, expected
root output and unchanged directory/detector/data inputs. Three CLI dependency
refusals pass. The initial CLI control omitted the existing required --build
argument and stopped at argparse; the corrected control passes, with the initial
log retained. This was a test command error, not an application failure.

One complete production Qt/helper job passes on the disposable 64-MiB simulated
USB, without host devices/root. Independent readback matches all seven files.
The prepatched mixed-case root loader keeps its source bytes and remains different
from the unchanged directory SETUPLDR. The differing 131,110-byte root detector
and 28-byte directory detector keep their individual hashes. The root SIF gets
the original insertion while preserving root-only content; all directory sources
and data/copy.bin remain unchanged. Copy accounting includes all extracted bytes.
Original masquerading MBR/PE PBR, geometry, backup record and separate FSInfo
hints pass. Nonwriting fsck emits only the precisely allowed original boot-label
warning (NO NAME in the original PBR versus RUFUS_TEST in the root); no repair
or other diagnostic occurs.

The existing Widgets guest holds launcher exit after the helper result, requires
Writing/Finalizing with progress below full, then releases it and requires clean
sync/unmount/helper+launcher exit, released claim and full green READY. Exactly
one simulated authorization is recorded; unplug preserves READY. Generated BIOS
reaches `RUFUS_WINDOWS_BOOTMGR_OK cs=2000 drive=81 tail=ok`, with unchanged snapshot
media. This qualifies the full Linux creation and generated loader handoff;
it does not execute the synthetic detector, qualify actual Windows Setup/host
polkit or add a Windows customization/installation test.

CP22n22's normal/sanitizer component and focused Qt evidence remains applicable
because production is byte-identical; no unnecessary repeat was run. CMake build,
fixture/CLI/syntax controls and the full guest pass. Guest input hashes verify
the actual helper and GUI binaries used. Evidence, eight-file baseline/diff,
production/source/binary hashes, compact guest manifests/logs and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n23`.
Verify exported evidence and remove the build/remaining guest directory; large
media is already removed by the recipe. Preserve user ISO and dependencies.

Next CP22n24: compare remaining mixed legacy loader dispatch with the original
Windows format/write sequence, then choose the smallest supported addition using
existing shared flags and boot providers. The root-file/provider audit already
completed in CP22n16/18 need not be repeated, and unrestricted/nonconverging root
loaders remain required parity work. Do not infer a Windows version from a loader
name or clear report flags merely to select an easier provider. Localized no-BOM
SIF encoding, compatible FAT16 handoff and Windows To Go remain required work.
Actual Windows validation and the one-customization testing limit are unchanged.


## CP22n24 — Follow the shared plan for mixed setup directories (2026-10-04)

The remaining mixed dispatch audit found one small, directly reusable addition:
a complete i386/AMD64 setup, known /minint options and only some files in minint.
Original GetWinPeSetupPlan selects WINPE_KEEP_LOADER for any minint file bits
with /minint, while still selecting the complete i386/AMD64 family. SetupWinPE
copies/retains that family's detector and loader, skips root SIF copying and
returns without loader patches. Without /minint the same mixed layout selects
WINPE_UNSUPPORTED_LAYOUT. Partial minint alone is not a complete setup family.

The native Linux preparer already implements this shared decision, including
an existing positive/refusal component pair. IsLegacyWinPeImage added an
independent blanket partial-minint rejection, preventing fresh preflight and Qt
from reaching it. Remove that duplicate restriction and use the original plan.
Only that predicate and its contract comments change in production; all other
692 production hashes match CP22n23. Original Windows/shared code, native file
preparation, Qt, helper/protocol/auth and boot providers remain unchanged. Keep
report masks, unknown version and all required-file/encoding/bounds/identity/
error checks. This is mixed setup-directory admission, not a mixed boot-provider
route or a guarantee of actual Windows Setup compatibility.

The broader BIOS mixed-loader dispatch cannot be represented by removing its
gates from the current Windows route:

| Original trigger | Original Windows sequence | Linux implication |
| --- | --- | --- |
| WinPE plus Syslinux, without Alt+E | Syslinux takes MBR priority; install Syslinux instead of WritePBR; still apply SetupWinPE after extraction for a BIOS target. | The current separate Windows/Syslinux routes do not perform this complete combination. Preserve the gate until provider selection and post-copy preparation are composed explicitly. |
| Windows plus Syslinux with Alt+E | Windows overrides the MBR preference; the Syslinux installation condition is false; WritePBR and the later BIOS WinPE fixup still apply. | Keep the original Windows/Alt+E exception. Do not globally give Syslinux precedence or clear its report bit. |
| WinPE plus GRUB | MBR priority and GetSecondaryBootType select GRUB; WriteSBR supplies its core; PBR handling and later BIOS SetupWinPE are separate steps. | A Windows request alone omits the needed core/provider work, and current final WinPE record installation assumes the pure legacy route. Share the actual dispatch before widening this combination. |
| Legacy plus EFI/install-archive companions | BIOS-only versus UEFI/Alt+E changes provider/layout work; original WIM-based EFI repair remains its separate source-content operation. | Keep the existing mixed/EFI/version gates rather than infer a known installer from names. |

References are the checked-out format.c WriteMBR/WriteSBR, the boot installation
branches and post-ExtractISO SetupWinPE call, plus the already shared setup plan.
CP22n16/18 root-file/provider priority evidence remains applicable and was not
reimplemented. A future shared dispatch must distinguish MBR, SBR, PBR and
post-copy operations; adding another broad Linux image-type abstraction is not
needed to express those original decisions.

Before the change, all 24 supported positive source rows fail at the old ENOTSUP
gate. Source tests now cover all six partial minint subsets with complete i386/
AMD64, /minint present/absent and ISO/UDF; each checks NTFS and advanced FAT32.
They verify exact report masks, original selected plan, unknown version, no root
SIF stage, unchanged ancillary files and no target writes. An obsolete refusal
is replaced by an actually incomplete whole setup. Native tests expand the same
layout table and assert untouched ancillary/source files, copied or retained
unpatched root loader, skipped root SIF and retained root metadata/no staging.

Final normal and ASan/UBSan/LeakSanitizer component suites pass: 838 source cases,
264 native cases and 66,040 shared checks, no failures/skips. Focused Widgets:
94 passed, no failures/skips, including 24 new partial-minint i386/AMD64 ISO/UDF
NTFS/advanced-FAT32 rows with absent or identical root loader. They retain the
shared request/defaults, plan/report checks, unknown version, one simulated
authorization and full READY assertions. The GUI helper is simulated; actual
native file preparation is component-tested separately. No full guest or further
Windows customization/installation test is claimed for this newly admitted
combination. Evidence, ten-file baseline/diff, production/source/binary hashes,
before/final logs and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n24`.
Verify exported evidence then remove both temporary builds.

Next CP22n25: extend the existing generated i386/FAT32 Qt/helper recipe with one
explicit partial-minint fixture setting, preserving its defaults. Generate known
/minint options and only an ancillary minint SIF while keeping i386 as the complete
selected family. Include an identical retained root loader/differing detector and
unused root SIF. Independent readback must prove unpatched loader/root SIF and
all directory/ancillary bytes, ordinary 0x80 drive behavior without masquerading,
original records and the existing one-authorization/durable READY checks. Reuse
its generated loader rather than add another job engine or Windows customization
test. Then address shared mixed-provider dispatch; localized no-BOM SIF, FAT16,
nonconverging roots and Windows To Go remain required parity work.


## CP22n25 — Complete partial-minint Qt/helper readback (2026-10-04)

The existing generated legacy Qt/helper recipe now has one opt-in partial-minint
setting. It requires a complete i386/AMD64 family, adds only an ancillary minint
SIF and puts /minint in the selected family's options. Distinguish the actual
loader directory from the shared keep-loader choice in fixture/readback logic.
Preserve existing defaults, including complete minint and prepatched roots;
refuse the prepatched choice with the new keep-loader fixture. This changes only
test recipes/documentation. All 694 production hashes match CP22n24.

Six fixture/hash controls cover original i386, prepatched i386/AMD64, complete
minint and partial-minint i386/AMD64. Six CLI dependency refusals run before output
creation. One full existing Qt/helper i386/FAT32 guest includes all three root
files: identical raw loader, differing 131,110-byte detector and distinct unused
root SIF. Independent eight-file readback equals the source manifest exactly.
Root/directory loaders remain raw, neither SIF is edited, and the ancillary
minint SIF, directory detector and copied data retain their exact hashes.

The selected Win7 MBR remains Win7; no masquerading is installed. Original PE
FAT32 PBR/geometry/backup/FSInfo checks pass. Nonwriting fsck reports only the
precisely allowed original missing boot-label versus RUFUS_TEST directory-label
notice, with no repair or other diagnostics. One simulated authorization, held launcher exit
before full READY, filesystem/device sync, private unmount, released exclusive
claim and completed READY preservation on unplug all pass. Generated BIOS
observes `cs=2000 drive=80 tail=ok`; snapshot media is unchanged. Actual synthetic
detector execution, Windows Setup and host polkit remain unqualified. There is
no additional Windows customization/installation test.

CP22n24's normal/sanitizer component suites and 94 focused Qt cases remain the
production baseline; no repeat is needed for unchanged production. The CMake
build uses Ninja default parallelism. Evidence, seven-file baseline/diff,
production/source/binary hashes, fixture controls, actual guest input hashes,
compact guest logs/manifests and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n25`.
Verify exported evidence and remove the build and remaining guest/controls;
large guest/media/readback copies are already removed by the recipe.

Next CP22n26: extract the smallest original boot-dispatch decision into common C
code for MBR, SBR and PBR selection, preserving Windows's Alt+E exception and
keeping BIOS WinPE post-copy preparation independent. First compare the shared
choice against the checked-out original with focused tables; use it in Windows
and existing Linux provider selection before admitting mixed providers. Do not
clear image flags, infer a Windows version or remove mixed-layout gates ahead of
the actual provider work. CP22n16/18 root-file audit remains applicable. Localized
no-BOM SIF, FAT16 handoff, nonconverging root loaders and Windows To Go remain
required parity work; actual Windows build/runtime validation happens later.


## CP22n26 — Share the original boot-provider dispatch (2026-10-04)

Move WriteMBR's ordered code choice and FormatThread's Syslinux/PBR operation
choice into the existing common format_policy C module. A small options struct
carries their original scalar inputs; record/operation enums describe only the
original branches. This adds no image-class framework, request fields, host
inference, native writer implementation or platform conditionals.

GetMasterBootRecord preserves protective GPT first, Windows/Alt+E BIOS override,
zeroed nonbootable/UEFI, Syslinux, GRUB2, Grub4DOS, standalone ReactOS, FAT
KolibriOS and Windows/Rufus fallback priorities. Needs-masquerading remains the
caller's existing shared setup-plan result. Windows WriteMBR retains its native
sector validation, LBA/active flag updates, original ms-sys writer/log calls and
disk notification. GetBootInstallType preserves nonbootable/UEFI:NTFS no-op,
UEFI sanity-check branch, standalone or image Syslinux/ReactOS installation with
the Windows/Alt+E exception, and the remaining PBR branch. Native operations,
error handling and later BIOS SetupWinPE remain where they were.

GetSecondaryBootType was already shared and stays unchanged. In particular,
Windows/Alt+E can choose a Windows MBR and PBR while the separate SBR selector
still chooses GRUB2. ReactOS image flags select the Syslinux installer even when
the MBR falls back to Windows. Do not collapse these separate decisions.

Linux FormatMbrBootCode translates each currently admitted single-provider
request into the common MBR decision and maps its supported records to native
writer values. This translation is not the full original ISO report and does
not enable mixed providers. All existing Windows/FreeDOS MBR choices, standalone
and image GRUB2/Syslinux, zeroed nonbootable/UEFI routes and late legacy
masquerading retain their behavior. Mixed report flags and provider composition
remain gated; do not represent them by deleting flags or guessing versions.

Four production files change: format_policy.c/.h, format.c and the Linux
format_options.cpp adapter. The other 690 production hashes are unchanged,
including native writers, helper/preflight, Qt, protocol, authorization, sync/
cleanup and the shared WinPE preparer. Existing Windows build files already
include format_policy; Linux stays CMake with Ninja default parallelism.

A frozen test-only copy of the pre-extraction Windows branches supplies the
reference, retaining original ordering/conditions. Only logging, native writer
calls and EFI sanity checks become returned choices. Across 512 report flag
combinations, all ten boot choices, four representative filesystems, BIOS/UEFI
and all GPT/Alt+E/Rufus/masquerade combinations, 655,360 pairs of decisions agree.
Explicit mixed MBR/SBR/install expectations supplement the reference. The full
shared format suite passes 1,311,943 checks normally and under ASan/UBSan/LSan.

All nine selected normal suites pass after correcting an old fixture expectation:
55 format-options, 45 FreeDOS-request, 71 Syslinux-request, 164 GRUB2-request,
68 Windows-request, 838 fresh-source and 264 native WinPE Qt cases, plus the
66,040 shared WinPE and 1,311,943 shared format checks. The new adapter control
checks every existing provider/default choice. The GUI and helper compile.
The eight initial Windows request failures used a six-byte "source" file but
expected to reach the target gate; the already-existing fresh source guard
correctly returns EIO first. Rename these rows invalid-source, assert EIO/no
target gate, and keep zero-start/zero-copy/untouched-target checks. Source/helper
production is unchanged; do not bypass or weaken its validation. Initial failure
logs and test baseline are retained. An initial build command used the wrong
GUI target name rufus-qt; correct it to the existing rufus-linux target.

No new USB guest, Windows customization or Windows runtime validation is claimed.
CP22n25 remains the preceding complete generated-job evidence; original Windows
build/runtime validation happens later on Windows. Compact baseline/diff,
production/source/binary hashes, reference extraction recipe, initial/final
normal and sanitizer logs, caches and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n26`.
Verify evidence and remove all three disposable build directories.

Next CP22n27: share WritePBR's actual filesystem/boot-record priority and use it
through the existing native FAT/NTFS adapters, preserving unsupported-record and
mixed-layout gates. This is separate from choosing whether to install Syslinux.
Then compose the smallest mixed provider with the original MBR/SBR/PBR/post-copy
sequence, retaining final WinPE record/drive behavior and the Linux 0x81 partition
parser constraint. Nonconverging roots, localized no-BOM SIF, FAT16 handoff and
Windows To Go remain required parity work.


## CP22n27 — Share the original partition boot-record choice (2026-10-04)

GetPartitionBootRecord now extracts WritePBR's actual-filesystem priority into
existing common format_policy C. FAT16 selects FreeDOS, standalone ReactOS,
unsupported image KolibriOS or standard records. FAT32 selects FreeDOS,
standalone ReactOS, image KolibriOS, BOOTMGR (including EFI-only BOOTMGR), complete
WinPE or standard records in that original order. NTFS selects its original
record; ext requires no PBR; other filesystems are unsupported. This chooses
record bytes only: GetBootInstallType still independently chooses whether the
job installs a PBR, Syslinux or performs the UEFI check.

Windows WritePBR consumes the shared choice while retaining native validation,
ms-sys writers, logging, physical drive ID, backup-sector loop and error handling.
Linux's existing FAT adapter maps its explicit writer modes into the same choice;
these are not inferred image versions. Final legacy BOOTMGR still uses the PE
writer with its existing FSInfo/drive fixup. The existing native NTFS adapter
checks the common NTFS choice before its original writer, preserving request,
table, device identity, exclusive claim and sync checks. No additional image
framework, protocol field, platform conditional or native writer is introduced.

Five production source files change: format_policy.c/.h, format.c, fat_boot.c
and prepare_device.cpp. The other 689 hashes in the existing 694-file production
inventory match. One existing Linux CMake linkage adds format_policy to the FAT
adapter. That build script is tracked separately from the source inventory.
Windows already builds the common module; Linux remains strictly CMake with
Ninja default parallelism. Mixed-provider admission, GUI and helper lifecycle
remain unchanged.

A frozen copy of the preceding Windows WritePBR selection branches compares
56,320 PBR choices across 512 report combinations, all ten boot choices and all
filesystem values including invalid sentinels. Explicit priority cases and
unchanged-report checks supplement it. The complete shared format suite passes
1,368,796 checks normally and under ASan/UBSan/LeakSanitizer. The reference is
test-only and its extraction reproduces identical bytes from the saved baseline.

All 13 selected normal suites pass: 403 Qt format/request cases, 615 native
FAT/NTFS installation/refusal/fault cases plus memory-bound/non-file guards,
and the shared format suite. All eight selected sanitizer suites pass with the
same native boot cases and shared checks. Native expected record bytes come
from original ms-sys arrays, independently of the new selector; geometry, backup,
FSInfo, short-I/O, readback, sync and cancellation checks remain. GUI/helper build.

The unchanged generated i386/partial-minint NTFS Qt/helper recipe passes a
complete job with the preferred new kernel ntfs driver. All eight target files
match source hashes, including retained raw root/directory loaders, differing
root detector and untouched root/selected/ancillary SIF files. Original Win7 MBR,
NTFS record/geometry and clean-filesystem readback pass. One simulated
authorization, held launcher exit before successful full READY, sync/unmount,
released claim and post-success unplug assertions pass. BIOS observes drive
0x80, intact loader tail and an unchanged snapshot. No additional Windows
customization qualification is performed.

The first ordinary TCG guest timed out after 180 seconds with only early kernel
messages and a power-off request; its cause remains unconfirmed. A diagnostic
retry adding serial tracing passed, then the original unmodified recipe passed.
Both successful runs and the initial failure are retained. No production or
repository guest-init workaround was applied. Actual NTDETECT/Windows Setup,
host polkit and Windows build/runtime remain unqualified and deferred.

Compact baselines/diff, production/source/binary hashes, reference extraction,
normal/sanitizer and initial/diagnostic/final guest logs, manifests and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n27`.
Export and verify evidence before removing all three builds and disposable guest
outputs. Preserve dependencies and the user's Windows ISO.

Next CP22n28: audit the smallest legacy WinPE/Syslinux mixture under Alt+E.
First follow the original independent MBR/SBR/installation/PBR decisions,
ExtractISO config edits and late WinPE fixup; admit it only when the entire native
sequence matches. Do not demand an unused Syslinux installer when the original
chooses Windows PBR installation, delete report flags, or guess a version.
Other mixtures stay gated. Nonconverging root loaders, localized no-BOM SIF,
FAT16 handoff and Windows To Go remain required parity work.


## CP22n28 — WinPE/Syslinux Alt+E backend preparation (2026-10-04)

Audit the checked-out Windows sequence rather than treating every reported boot
loader as an installer to run. For complete legacy WinPE plus Syslinux under
Alt+E and BIOS, GetMasterBootRecord chooses Windows/Rufus, GetBootInstallType
chooses PBR installation, and GetSecondaryBootType remains BT_IMAGE (no GRUB
SBR). WritePBR independently chooses FAT32 NT or PE according to the original
BOOTMGR flag. ExtractISO still patches configuration labels and creates, keeps
or backs up root syslinux.cfg. Later SetupWinPE independently prepares the root
loader/SIF/detector. Syslinux installation is skipped; its data does not need a
compatible native installer or an invented version. Optional old-C32 replacement,
persistence and EFI symlink work are not newly qualified by this checkpoint.

The existing legacy candidate predicate now accepts an explicit Alt+E argument
and uses the shared installation choice for a Syslinux companion. Fresh backend
preflight passes the existing request.enhancedInstall preference. Its existing
advanced FAT32/MBR/BIOS rules still apply; enhanced NTFS is not admitted here.
Other loader mixtures, incomplete/ambiguous sources, customization, unsupported
text, filesystem/space/ownership/cancellation and target checks remain. No report
flag is erased and unknown Windows/Syslinux versions remain unknown. The existing
extraction and WinPE preparer implement the combined file operations without
new writers or compatibility code. Only iso_write_check.cpp/.h change in the
694-file production source inventory; the other 692 hashes match, including
all original Windows/shared code, Qt, helper, native writers, protocol and build
files. No new host detection or privilege request is added.

Qt and other default candidate callers keep the original single-provider gate.
The helper already explicitly opts into fresh legacy preflight; this step admits
its backend prerequisites, but does not qualify a complete mixed device job.
Do not expose this mixture through Qt yet. In particular, Alt+E/NTFS preference
transport and filesystem transitions must be audited before GUI admission.

Thirty-six new real ISO/UDF functional rows cover i386/AMD64/minint, Win7/Rufus
MBR choices and absent/retained/overwritten root Syslinux configs. They perform
fresh retained-source preflight, actual ExtractIsoTree and PrepareWinPeRootFiles
on disposable directories, then compare every output file against independent
expected bytes. They check label edits, generated configs, patched config backups,
loader CRC/path/drive edits, SIF insertion, minint retention, preserved directory
inputs and existing root loader spelling. Shared initial/final record and
independent operation choices are checked. Unknown Syslinux has no loader version
and remains zero/uninferred; Windows metadata stays unknown and no WIM is opened.
Default GUI/unopted preflight remains gated, with no target access/authorization.
Twenty new ISO/UDF negative rows retain Alt+E-off, enhanced-NTFS, other providers,
EFI, customization, malformed config, symlink and partial-layout refusals.

The initial new fixture mistakenly retained an NTFS-sized cluster on a small
FAT32 partition and failed formatting checks. Correct the fixture to the existing
512-byte FAT32 cluster; a valid-fixture run against saved production baseline
then shows all 36 positive rows failing specifically at the old mixed-loader
gate. After the change they pass. Broad regressions expose two old FAT32/Alt+E
existing-BOOTMGR/Syslinux expectations for ENOTSUP; they now assert successful
preflight with untouched source/root input, while ordinary NTFS remains refused.
Initial failure logs are retained; no formatter or production workaround is used.

The six selected normal suites pass across the initial five successful suites
and the final changed-source rerun: 894 source cases, 264 native WinPE cases,
243 native ISO/UDF extraction cases, 68 Windows-request cases, 66,040 shared
WinPE and 1,368,796 shared format checks. Four selected sanitizer suites pass
across the initial three successful suites and final source rerun: the same
894/264/66,040/1,368,796 cases/checks under ASan/UBSan/LeakSanitizer. No failures
or skips remain. GUI/helper compile. No USB guest, BIOS boot, actual Windows
Setup, detector execution, host polkit or further customization qualification
is claimed for this mixed route; prior single-provider evidence remains intact.

Compact baseline/diff, production/source/binary hashes, initial/final normal and
sanitizer logs/caches, audit and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n28`.
Verify exported evidence and remove both disposable builds and temporary log/
script copies; retain user ISO and reusable dependencies.

Next CP22n29: qualify this advanced FAT32 mixture through a complete simulated
USB/helper job, checking independent MBR/PBR/final WinPE record/drive bytes,
extracted/config/WinPE file readback and sync/unmount/exit before READY. Preserve
Qt's gate until that job and its selection/settings behavior are qualified.
Then audit Alt+E NTFS and GUI transitions without adding another client image
classification or guessing versions. Other mixtures, nonconverging root loaders,
localized no-BOM SIF, FAT16 handoff and Windows To Go remain parity work.


## CP22n29 — Complete mixed WinPE/Syslinux FAT32 helper job (2026-10-04)

The generated i386/Alt+E FAT32 mixture now passes a complete simulated USB job
through the existing PrepareImageProcess client and helper. Only three VM test
files change: the recipe adds an optional Syslinux config fixture, the fixture
supplies independent expected file bytes, and the guest checks that native
Syslinux installers are absent and no Syslinux installation is logged. All 694
production source hashes match CP22n28. There is no new writer, classification,
request field, privilege flow or GUI admission.

Two default source/target manifest controls match the preceding fixture exactly.
Three fixture controls cover config creation, retention and overwrite/backup;
five invalid CLI controls were attempted here. CP22n31 later found that their
commands omitted required --kernel, so this checkpoint does not qualify the
intended mixed-option refusal messages; CP22n31 supplies corrected controls. The first
baseline control failed because its relocated Python source could not find the
unchanged adjacent assembly resource. Resolving that resource against the real
source directory fixes the control harness; initial and passing logs are retained.

The full guest qualifies the overwrite case with a prepatched mixed-case root
loader, differing retained detector and separate root SIF. Nine source files
yield ten target files, all matching independent expectations: patched Isolinux
label, generated root config, patched backup, prepared SIF and preserved loader,
detector and directory inputs. Unknown Syslinux stays unknown. Both native
Syslinux installers are absent in the guest; the unknown-version scan message
is present and no installation is attempted.

Independent readback checks the original Windows/Rufus MBR priority, zero SBR
gap, original PE FAT32 records and backup, BPB geometry, FSInfo and final drive
0x81 masquerade. Generated BIOS reaches the loader with drive 0x81 and intact
tail without changing the snapshot. The nonwriting fsck check has only the
precisely allowed original boot-sector/root-directory label notice; no repair
is applied. One simulated authorization, progress/phase ordering, source identity,
sync, private unmount, released exclusive access and helper/launcher exit before
client completion pass. The first ordinary TCG run passes without a guest-init
or production workaround.

Helper/client and required fixture targets build with Ninja default parallelism.
No CTest or sanitizer rerun is claimed here: production is unchanged and CP22n28's
normal/sanitizer component qualification remains the baseline. This job uses the
backend client, not the Widgets GUI. Qt admission, its green READY/notification/
unplug behavior for this mixture, actual detector execution, Windows Setup and
host polkit remain unqualified. No additional customization test is run.

Compact baseline/diff, source/production/binary hashes, control/build/guest logs,
independent manifests, boot bytes and reproduction are retained in
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n29`.
Verify exported evidence, then remove the disposable build and remaining guest
output. The recipe already removes large guest/media/readback copies. Preserve
the user's ISO and reusable dependencies.

Next CP22n30: audit and implement the mixed Alt+E NTFS backend using the existing
enhancedInstall preference/request fields. It is still gated; follow the original
independent installation/PBR/WinPE choices and final drive behavior, including
native NTFS parser constraints. Start with focused source/native checks, then
qualify a complete NTFS job. Afterwards qualify Qt's Alt+E toggle, filesystem and
request transitions before admitting the mixture. This checkpoint assumed NTFS
would remain the mixed image default. CP22n32 corrects that assumption: loading
with Alt+E already enabled follows the shared FAT32 preference; toggling preserves
a still-allowed filesystem. Qualify both filesystems before GUI admission. Do not
invent a loader version or duplicate the client image classification. Other provider mixtures and the
remaining legacy/Windows To Go parity work stay documented.


## CP22n30 — Mixed WinPE/Syslinux Alt+E NTFS preflight (2026-10-04)

The original Alt+E Windows exception chooses PBR installation on NTFS as on
FAT32, independently of the Syslinux report flag. WritePBR selects the original
NTFS record, then ExtractISO edits the configs and SetupWinPE prepares the root
files. Existing Linux prepareNtfs already writes that original record before a
fresh private mount, copies through the ordinary extractor, prepares WinPE and
syncs/unmounts before the final MBR masquerade. Keep active byte 0x80 through
Linux partition refresh/mounting; write the original final 0x81 only afterwards
without another rescan. No new NTFS writer or Syslinux installation is needed.

Remove the legacy preflight restriction that forbade enhancedInstall on NTFS.
Plain NTFS remains supported for single-provider legacy sources; mixed Syslinux
still needs explicit Alt+E through the existing shared installation selector.
Advanced FAT32, MBR/BIOS, no UEFI:NTFS and no modern customization requirements
remain. Other provider mixtures, incomplete/ambiguous files and ordinary source/
space/text/identity checks stay gated. Only iso_write_check.cpp changes in the
694-file production inventory; 693 hashes match, including Windows/shared code,
Qt, helper, protocol, native writers and build. No version is inferred, source
flag removed, new request field, detection or privilege prompt added.

Extend the existing real ISO/UDF functional cases with 36 NTFS-request rows for
i386/AMD64/minint, config creation/retention/backup and both MBR choices. They
combine fresh preflight, actual extraction and native root preparation in
disposable directories, comparing every file with independent expected bytes
and rechecking untouched source. NTFS uses the shared NTFS PBR choice; MBR/SBR/
installation and final WinPE choices remain independent. Unknown Syslinux and
Windows metadata, no WIM opens, no target access/authorization and default Qt/
unopted gates remain checked. Sixteen additional single-provider Alt+E NTFS rows
cover the existing setup/options/layout combinations. Mixed refusal coverage now
checks both filesystems: Alt+E off, UEFI:NTFS, other providers, EFI, customization,
malformed config, symlink and partial layout. The former enhanced-NTFS refusal
becomes a real UEFI:NTFS guard, and the former single-provider Alt+E refusal is
replaced by unsupported-exFAT coverage.

Before changing production, all 52 new positive NTFS rows fail specifically at
the old plain-NTFS gate while 68 existing positive rows and two lifecycle cases
pass. After the change, eight selected normal suites pass: 966 source, 264 native
WinPE, 243 extraction, 68 Windows-request, 47 NTFS-format and 29 NTFS-PBR cases,
plus 66,040 shared WinPE and 1,368,796 shared format checks. GUI/helper build.
Six selected ASan/UBSan/LeakSanitizer suites pass with the same source/WinPE/NTFS/
shared counts. The initial sandboxed sanitizer run fails because LeakSanitizer
cannot inspect its processes, also failing fixture subprocesses. An unchanged-
binary rerun outside the sandbox passes with leak checks still enabled. Preserve
both logs; no production fix, leak-check suppression or test-expectation workaround.

This is source/native component qualification, not a complete mixed NTFS USB
job or Widgets admission. Filesystem creation/PBR tests use disposable formatted
files; combined extraction/root tests use directories. Kernel NTFS mounting,
final mixed drive 0x81 handoff, complete sync/unmount/exit and Qt READY/unplug for
this route still need the next checkpoint. Actual Windows Setup/detector/polkit
and additional customization qualification remain deferred.

Compact baseline/diff, source/production/binary hashes, before/normal/sanitizer
logs and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n30`.
Verify exported evidence and remove both disposable builds and temporary files;
retain the user's ISO and dependencies.

Next CP22n31: extend the existing backend VM recipe to this mixed NTFS request,
retaining explicit Alt+E without UEFI:NTFS. Qualify one complete generated helper
job with independent config/WinPE readback, original MBR/NTFS records, final drive
0x81, preferred native NTFS mounting and sync/unmount/released claim/helper exit.
Do not expose the mixture in Qt until that job and its Alt+E/filesystem/request
transitions are qualified. Reuse the current components and avoid a second image
classification or guessed installer version.


## CP22n31 — Complete mixed WinPE/Syslinux NTFS helper job (2026-10-04)

The first ordinary TCG run passes the complete generated i386/Alt+E NTFS mixture
through the existing PrepareImageProcess client and helper. Only two VM test
files change: mbr.py accepts the existing mixed fixture option with NTFS, and the
guest freezes enhancedInstall for that explicit mixture without UEFI:NTFS. Its
existing Syslinux absence checks now also require the NTFS Extlinux installer to
be absent. Existing defaults, WinPE fixture bytes, guest init and all 694 production
hashes remain unchanged. No formatter, writer, classification or protocol is added.

Independent readback matches all ten target files from nine source files:
label-patched config, generated root config and patched backup, prepared root SIF,
prepatched mixed-case root loader and differing retained detector, with original
directory inputs untouched. Syslinux remains unknown, no Syslinux/Extlinux
installation is attempted and neither provider is present. The preferred kernel
ntfs driver is selected explicitly and confirmed by the production helper log.
The guest contains the existing NTFS-3G fallback packages but does not use them;
this checkpoint does not newly qualify fallback execution for this mixture.

Existing independent original-array checks verify the final Rufus masquerading
MBR, zero SBR gap, primary NTFS executable record, original backup/BPB geometry,
volume label and clean 0x0000 volume flags. Generated BIOS reaches the loader
with drive 0x81 and intact tail, leaving its snapshot unchanged. One simulated
authorization, copy phase/progress/source identity, sync/private unmount, released
exclusive claim and helper/launcher exit before client completion pass. The final
active byte is deferred until after unmount as in the existing NTFS path. There
is no diagnostic retry, timeout change or guest-init workaround.

Eight invalid CLI combinations are checked with required base arguments and
specific refusal messages before build/output access: missing route/layout/FS,
GUI/root-write faults, dual mode, both filesystems and missing NTFS packages.
The saved baseline refuses this now-qualified NTFS mixture at its FAT32-only
guard. The first control attempt omitted required --kernel and stopped too early;
correct that harness and retain both logs. Inspection also finds the same omission
in CP22n29's CLI controls, whose loose option-name assertion matched usage text.
Correct that historical qualification claim in the plan/testing notes without
rewriting its saved evidence. Its full FAT32 job and byte controls remain valid.

Helper/client/fixture targets build with Ninja default parallelism. Production is
unchanged, so CP22n30's eight normal and six sanitizer component suites remain
the baseline; no new CTest/sanitizer run is claimed. This is a backend client job,
not Widgets admission or full green READY/notification/unplug qualification for
the mixture. Actual Windows Setup/detector execution, host polkit and additional
customization qualification remain deferred.

Compact baseline/diff, production/source/binary hashes, control/build/guest logs,
independent manifests and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n31`.
Verify evidence and remove the disposable build and remaining guest output; the
recipe already removes large guest/media/readback copies. Preserve user ISO/deps.

Next CP22n32: audit and qualify Qt's mixed-image admission, Alt+E toggle, original
preferred filesystem and filesystem/request transitions using the existing report
and shared provider choices. Keep ordinary Alt+E-off mixtures gated. Do not erase
Syslinux flags, infer a version or add another client classifier. Once those
selection/settings cases pass, qualify the existing Widgets creation/READY/unplug
path for an admitted mixture before declaring GUI parity for it.


## CP22n32 — Mixed WinPE/Syslinux Qt admission and durable READY (2026-10-04)

Expose the qualified Alt+E FAT32/NTFS mixture through the existing Windows BIOS
candidate predicate. Its optional mixed-provider argument delegates to the same
legacy/shared installation choice; Qt supplies its saved Alt+E preference. Keep
ordinary mixed ISO admission gated. Retain enhancedInstall for legacy NTFS
requests, now qualified by CP22n30/31. Three production files change; 691 of 694
production hashes match. No new classifier, formatter, boot writer, helper
protocol, Windows edit or installation abstraction is introduced.

Loading this mixture with Alt+E already enabled follows the shared FAT32/512-byte
cluster preference. Choosing NTFS uses 4096-byte clusters. Toggling Alt+E retains
a still-allowed filesystem; reloading restores the shared image preference.
Keep MBR/BIOS, offer only the qualified FAT32/NTFS choices, reject FAT16/GPT and
hide modern installation options. The saved preference persists to a new session.
Root BOOTMGR, WinPE and Syslinux report flags remain intact. Windows and detected
Syslinux versions remain unknown. The original GUI scanner may still print its
embedded fallback diagnostic, with sl_detected_version zero; this is not a newly
inferred image version or selection of a native installer. Fresh creation preflight
selects the Windows path with sl_version zero/uninferred and no Syslinux tools.

New hybrid transition cases expose a real Qt defect: optional unknown Windows
metadata returned no write choices before DD filtering for a mixed root-BOOTMGR
image with Alt+E off. Permit that optional metadata using the existing qualified
legacy candidate facts; ISO admission still requires Alt+E. A valid hybrid now
retains DD and shows the existing ISOHybrid notice. After its dismissal, Alt+E
can restore ISO creation. No hybrid signature or image facts are fabricated.
An initial test driver then failed by pressing Alt+E while the modal notice was
still open; correcting that driver, rather than product shortcuts, resolves it.
The initial build also found missing isoWriteChoices arguments in the new test;
correct them. Retain the initial and final test logs.

Extend the existing source/Widgets fixtures and VM driver. The focused Widgets
set passes 167 cases normally and under ASan/UBSan/LeakSanitizer: 120 request rows
include 48 mixed ISO/UDF/family/root/filesystem cases, four Alt+E/load/reload/hybrid
transitions, ten extra mixed invalid-root refusals before authorization, and
existing legacy/modern-Windows controls. Check confirmation details, frozen
requests, no modern customization/no Syslinux tools, mock helper once, untouched
mock target, empty idle READY, full completed READY and one notification.
Source admission/extraction suite passes 966 cases normally and under sanitizers;
add explicit opt-in/default Windows candidate assertions to the existing mixed
functional rows. Normal image analysis (46), format choices (55) and Windows
requests (68) also pass. Leak checks remain enabled outside the sandbox needed
for private sockets/process inspection. Shared original policy/writer code is
unchanged; CP22n30's deeper shared/native component baseline remains applicable.

A complete generated i386/mixed/prepatched-root/differing-detector/root-SIF
Widgets NTFS guest passes on the preferred kernel ntfs driver. It loads with
Alt+E, shows the shared FAT32 default, explicitly selects NTFS and confirms
MBR/NTFS/BIOS before one simulated authorization. The actual production helper
copies nine source files into ten independently expected target files, including
label edits, generated Syslinux config/backup and prepared WinPE SIF. Original
Rufus/NTFS records, backup/BPB geometry, clean volume flags, zero SBR gap and
BIOS drive 0x81/tail/snapshot checks pass. Both FAT Syslinux installers and NTFS
Extlinux are absent and unused. Hold the completed helper launcher before exit:
Qt stays Finalizing below full progress, then reaches full green READY only
after release, sync/private unmount, released exclusive claim and process exit.
Unplug preserves READY. Both the first run and the final-binary run after the DD
fix pass; the latter's source/binary hashes are verified. No timeout/init change. Seven remaining invalid CLI combinations refuse before
build/output access with specific messages; the saved baseline reproduces its
now-superseded GUI refusal. CP22n31's historical GUI guard claim is superseded
by this qualification.

This qualifies the mixed GUI request and one complete NTFS Widgets job. CP22n29
remains the full mixed FAT32 backend qualification; the new Widgets request rows
exercise that filesystem without claiming another full FAT32 guest. Actual
Windows Setup/detector execution, host polkit and mixed NTFS-3G execution are not
newly qualified. No further customization/installation tests are scheduled.

Compact baseline/after/diff, production/source/binary hashes, normal/sanitizer
logs, both guest readbacks/manifests and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n32`.
Verify evidence, then remove disposable builds and remaining guest output; the
recipe already removes large disks/readback copies. Preserve user ISO/deps.

Next CP22n33: audit the original WinPE/GRUB provider combinations against the
shared independent MBR/SBR/installation/PBR/post-copy decisions and the current
native sequence. Select one coherent reusable route for the next small checkpoint;
keep unqualified combinations gated. Do not treat successful config copying as
bootloader compatibility. Localized no-BOM SIF, compatible FAT16, nonconverging
roots and Windows To Go remain required parity work.


## CP22n33 — Reuse the SBR-free WinPE/Syslinux/GRUB path (2026-10-04)

Audit the original WriteMBR/WriteSBR/installation/WritePBR/SetupWinPE sequence.
Alt+E bypasses other BIOS providers when choosing the Windows MBR and suppresses
Syslinux installation; it does not suppress GRUB SBR selection. Without Syslinux,
WinPE/GRUB2 and WinPE/GRUB4DOS still request their SBR under Alt+E. With Syslinux
present, original GetSecondaryBootType precedence returns BT_IMAGE, meaning no
GRUB SBR. The existing Alt+E Windows PBR/extraction/WinPE path then covers GRUB
files as ordinary copied data. This is a coherent extension of CP22n32, not a
new GRUB installer or a blanket admission of every GRUB mixture.

The existing IsLegacyWinPeImage gate now names grubNeedsSbr using the shared
secondary-boot choice instead of rejecting every HAS_GRUB report. Preserve its
other report/source/provider constraints and the saved Alt+E requirement. Qt and
fresh preflight use the same existing predicate. Only iso_write_check.cpp and its
commented header change in production; 692 of 694 hashes match. No helper,
protocol, original Windows/shared algorithm, writer or privilege change.

Twenty-four new positive real ISO/UDF functional rows fail at the old blanket
gate before the change; its 72 existing rows pass. After the change, GRUB2,
GRUB4DOS and both providers alongside Syslinux cover i386 and keep-minint,
FAT32/NTFS and both views. Retain every provider/version fact; detected Syslinux
and Windows remain unknown, while a recognized GRUB2 module remains 2.12.
Compare every extracted/prepared file with independent expected bytes: label-
patched grub.cfg, unchanged normal.mod/grldr/menu.lst, existing Syslinux config/
backup and WinPE root preparation. menu.lst is not an applicable original config
patch target and keeps its CRLF/source label. Check shared Windows MBR/PBR and
absent SBR versus ordinary Alt+E-off Syslinux installation, default/unopted gates,
no WIM/device access and no source mutation.

Replace the obsolete all-filesystem GRUB refusal with actual GRUB-only SBR
refusals and original FAT-less/NTFS-less restrictions. Twelve pure-GRUB ISO/UDF/
filesystem rows stay gated for GRUB2, GRUB4DOS or both; capability restrictions
reject the missing filesystem and four added positive rows admit the other one.
The full source suite passes 1,006 cases normally and with ASan/UBSan/leak checks.
The focused Qt set passes 198 cases normally and under the same sanitizers:
16 additional request rows, four added load/hybrid/Alt+E transitions, ten added
invalid-root refusals before authorization, and one added GRUB-prompt Cancel.
Existing source/shared/native writer baselines remain; no unchanged million-case
policy or formatter rerun is claimed.

The audit also confirms the original pre-write GRUB version lookup remains
independent of SBR/installer choice. Existing Qt performs it even for this
SBR-free mixture. Initial Widgets and guest drivers stopped at SelectingGrubCore
because the 2.12 fixture differs from the embedded provider. Fix only those
drivers: exercise the existing Yes/No/Cancel dialog, choosing No for creation
and Cancel for refusal. Keep the prompt before confirmation/authorization.
An accepted fallback selects no core bytes or core request for this Windows
route. No network download is performed or existing prompt bypassed. Retain
initial failed driver logs and the successful corrected runs.

A complete generated mixed i386/GRUB2/GRUB4DOS/Syslinux NTFS Widgets job passes
after answering No. Thirteen source files become fourteen independently checked
target files: prepared WinPE roots, Syslinux patches/config/backup, GRUB config
label edit and preserved modules/loader/menu. Fresh report flags and the shared
absent-SBR decision are checked in the guest. No Syslinux/Extlinux provider is
present or installed, no GRUB installation/core is requested, and the entire
embedding gap stays zero. Original final Rufus/NTFS records, backup/BPB geometry,
clean volume flags, preferred kernel ntfs, BIOS drive 0x81/tail/snapshot, one
simulated authorization and held-exit Finalizing/full READY/unplug all pass.
Six specific invalid VM CLI combinations refuse before build/output access.
There is no formatter, timeout or guest-init workaround.

This qualifies one complete combined NTFS Widgets job and the FAT32/source/view/
provider component routes, not another full FAT32 guest or GRUB-only handoff.
Actual Windows Setup/detector, host polkit, mixed NTFS-3G execution and another
customization/installation matrix remain deferred.

Compact baseline/after/diff, production/source/binary hashes, old-gate failure,
normal/sanitizer/control/build logs, failed/successful guest manifests and exact
readback, plus reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n33`.
Verify evidence then remove both builds and remaining guest output; the recipe
already cleans large media on success and failure. Preserve user ISO/deps.

Next CP22n34: qualify the independent GRUB2 SBR write separately from MBR choice.
The native WriteGrub2CoreImage currently requires a GRUB2 MBR, while the original
Alt+E Windows path still writes a GRUB2 SBR beside its Windows MBR. Reuse its
bounded/sync/readback writer with explicit original record choices before
changing request/helper admission. Ordinary GRUB-only WinPE also requires a
separate audit of final MBR/active-byte behavior; keep it gated. GRUB4DOS SBR,
localized no-BOM SIF, compatible FAT16, nonconverging roots and Windows To Go
remain required work.


## CP22n34 — Independent GRUB2 SBR with original Windows MBRs (2026-10-04)

Windows WriteSBR selects its provider independently of WriteMBR. In particular,
Alt+E can choose Windows code while still writing the selected GRUB2 core at byte
512. Reuse WriteGrub2CoreImage for the selected original GRUB2, Rufus, Win7 or
Rufus-masqueraded MBR; reject Syslinux and zero-code choices. Keep the existing
CheckMbrImage before/after checks, exact capacity/identity and filesystem/layout
constraints. A matching Windows MBR does not imply that it executes this core.
Only partition_image.c and its contract change in production; 692 of 694 hashes
match. No image admission, protocol, helper sequencing or original Windows/shared
algorithm changes. Payload version/module compatibility remains the caller's job.

The saved old writer refuses all nine Windows-record/FAT16/FAT32/NTFS choices
without writing. A new positive Win7/FAT32 case fails at that production gate
before the change. The extended functional suite passes 643 cases normally and
with ASan/UBSan/leak checks on disposable regular files. Independently compare
the original MBR bytes, signature, active/type fields and partition extents using
the existing original-header/sfdisk inspection. For every accepted record and
filesystem, compare the whole target before/after: only actual core bytes at
512 may change, without padding or changing the remaining gap/data. Verify
unchanged source payload, descriptor flags/position/count, short I/O/EINTR,
partial failure and secondary flush errors, cancellation, complete readback,
post-write target/layout changes, malformed records and exact gap/chunk bounds.
Cross-check all different accepted record choices: a Rufus/masquerade active-byte
mismatch still refuses before writing. Valid Syslinux/zero-code layouts refuse.

The 28-test normal set also covers the twelve accepted MBR/filesystem choices'
existing image/preparation regressions, ext2/ext3 GRUB2 layout and existing core
source/helper image checks. The eight-test sanitizer set repeats the complete
643-case core suite, GRUB2 FAT32 and masqueraded NTFS image/preparation, ext layout
and core source/helper image checks. The broader helper test initially has twelve
stale refusal expectations for previously qualified older-Windows FAT32/dual
routes. A separately compiled baseline writer/helper reproduces all twelve
failures. Correct the obsolete refusal/start expectations and explicitly assert that Windows fixtures
do not call the GRUB core writer; keep all existing original-record, payload and
copy readbacks. All 290 helper-image and 44 core-source cases then pass normally
and under sanitizers. Save the failures and differential build recipe.
Leak checks stay enabled. Native helper and
test targets build with Ninja's default parallelism. No guest boot or new USB,
GUI admission, Windows Setup/customization or host authorization is qualified by
this low-level checkpoint; the previous complete paths retain their evidence.

Compact baseline/after/diff, production/source/binary hashes, old-gate controls
and positive failure, build/normal/sanitizer logs and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n34`.
Verify evidence before removing both disposable builds. Preserve user ISO/deps.

Next CP22n35: connect one coherent GRUB2-SBR/Windows-MBR image route through the
existing selection and source validation. Keep the fresh shared MBR/SBR/PBR and
WinPE plan independent; preserve the original GRUB version lookup and selected
payload fingerprint. Audit reuse of the current request's core fields and helper
load/install operations before adding any protocol or adapter. Keep all new image
routes gated until request and helper sequence qualification. Ordinary WinPE/GRUB
without Alt+E additionally needs final MBR/active-byte analysis. GRUB4DOS SBR,
localized no-BOM SIF, compatible FAT16, nonconverging roots and Windows To Go
remain required work.


## CP22n35 — Reuse Windows requests and retained GRUB2 SBR payloads (2026-10-04)

Choose one coherent backend route: NTFS, Alt+E and the original WinPE keep-loader
plan, with fresh shared dispatch selecting a GRUB2 SBR. The Windows operation
continues choosing its original Rufus/Win7 MBR and NTFS PBR. Reuse the existing
optional core path/size/SHA256 fields, private staging and immutable owned snapshot;
do not add another operation, installer flag or compatibility abstraction. Fresh
preflight must select BT_GRUB2 independently of the MBR. Reject a supplied core
for no GRUB, Syslinux precedence, a GRUB4DOS SBR or a modern Windows source.
Legacy source checks still enforce complete/known setup options, filesystem and
root contracts. Default/Qt candidates keep the SBR gate. FAT32, patch/masquerade,
Alt+E-off, dual EFI, customization and other new routes remain gated.

The native NTFS helper reuses loadGrub2Core/installGrub2Core between formatting
and its existing NTFS PBR/copy/WinPE preparation. Selected bytes are retained
before target access and never reopened during writing. Check the fresh Windows
source before opening/unmounting the target. Keep one existing authorized job,
flush/readback and final sync/private cleanup/exit behavior. Payload fingerprints
prove byte identity, not core/module compatibility or authenticity; production
payload selection and actual boot qualification remain required. Unknown Windows
metadata stays unknown and no native Syslinux version/installer is inferred.
Six existing production files change; 688 of 694 hashes match. The original
Windows/shared policy and CP22n34 writer remain unchanged.

Twenty-eight new ISO/UDF source cases cover both Windows MBR choices: eight
keep-loader GRUB2/both-GRUB positives and twenty no-core/no-GRUB/Syslinux/GRUB4DOS/
patch refusals. Check fresh flags/version, unknown Windows metadata, independent
MBR/SBR/PBR dispatch, backend opt-in, default Qt refusal, no target/WIM access and
source preservation. The complete source set passes 1,034 cases. The retained
core set passes 45, including a Windows owned-snapshot case. Windows request/
process checks pass 73: reuse the original operation, round-trip core metadata,
reject invalid layouts/fields/customization and stage bytes in the existing
one-authorization lifecycle, including held child exit. A separately linked
unchanged protocol reproduces the new request's refusal and all four staged
process failures, without replacing current binaries.

The native-helper regular-file fixture adds nineteen cases. Six successes cover
ISO/UDF, both original MBRs and later core-path substitution; retain the original
700-byte placement fixture snapshot. Compare original MBR/active/type/signature,
core and untouched remaining embedding gap, original NTFS PBR, generated BOOTMGR/
ntdetect.com, absent root SIF and label-patched grub.cfg. Check one core write
before PBR/mount/copy, ordinary progress and finalization. Faults cover core,
format/PBR/mount/copy/sync/cleanup, bad payload hash, source mutation and copy
cancellation. No-GRUB/Syslinux/patch sources refuse before target access; late
prewrite source mutation refuses before writes, after read-only target opening.
The complete helper-image set passes 309 cases; the existing GRUB request set
passes 164. All five suites pass normally and with ASan/UBSan/leak checks.

Retain initial new-test failures: the detector's expected spelling was uppercase,
while original preparation writes lowercase ntdetect.com on this case-sensitive
fixture; another new assertion confused before-write with before-target refusal.
Correct only those expectations, preserving production behavior and the source/
target-access distinction. No sanitizer error is suppressed. This qualifies the
regular-file backend sequence and process lifecycle, not a new GUI route, physical
USB/guest boot, host polkit or Windows Setup/customization qualification.

Compact baseline/after/diff, production/source/binary hashes, differential old
protocol control, normal/sanitizer/build logs and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n35`.
Verify evidence, then remove disposable builds/control binaries. Preserve ISO/deps.

Next CP22n36: connect the qualified NTFS keep-loader/SBR route to Qt's existing
GRUB version lookup and confirmation flow. Audit how source checks can precede
payload selection without guessing versions or conflating MBR and SBR. Preserve
the original Syslinux-suppressed lookup behavior and Cancel before authorization.
Qualify selection transitions and one complete simulated USB helper/READY job
before enabling the route. FAT32 and patch/masquerade SBR sequencing, GRUB4DOS SBR,
localized no-BOM SIF, compatible FAT16, nonconverging roots and Windows To Go
remain required work.


## CP22n36 — Source checks before GRUB payload selection (2026-10-04)

Split the planned Qt integration at its ordering checkpoint. Qt performs fresh
source preflight before Rufus's GRUB version lookup, but CP22n35's final checks
require an already selected core. Add CheckIsoWriteSource as a read-only entry
into the same validation implementation. Only that entry permits an unselected
core for NTFS/Alt+E WinPE with the original keep-loader plan and fresh BT_GRUB2
SBR dispatch. Qt's initial check uses it. No dummy core, guessed version, new
request flag/operation or duplicate source scanner is introduced.

CheckIsoWriteImage and CheckLegacyWinPeImage keep their final payload gate; the
helper still calls those before target access. Reuse all existing source ownership,
retention, root/options, extraction-space, filesystem and partition checks.
Unknown Windows/GRUB versions stay unknown. Invalid explicit core metadata and
cores suppressed by Syslinux or mismatched with GRUB4DOS/no-GRUB still refuse.
Only three production files change; 691 of 694 hashes match. Original Windows,
shared policy, protocol, staging, helper and writer are unchanged. Qt selection
remains gated until selected/embedded payload freezing and full job qualification.

Eighty added ISO/UDF cases across both Windows MBR choices cover selected and
unselected source checks, both-GRUB precedence, unknown versions, invalid settings,
root/options faults, missing detector, incomplete analysis, read/cancel/source-change
errors and mismatched core dispatch. Verify source preservation, absent target,
retained-view ownership/descriptor cleanup, legacy opt-in and caller ownership.
Unselected successes still fail final preparation. A private unchanged-source
control refuses all twelve new unselected positives; four selected controls pass.

The complete source suite passes 1,114 cases and existing request/process suite
73, normally and with ASan/UBSan/leak checks. The 227 focused existing Widgets
cases cover WinPE/Alt+E/filesystem/hybrid transitions, invalid-root refusal and
GRUB lookup/creation regressions under both builds. Native helper builds. No new
full USB/boot, host authorization or customization qualification is claimed.
Compact sources/diff, manifests, baseline control, build/test logs and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n36`.
Verify evidence before removing disposable builds/control files; preserve ISO/deps.

Next CP22n37: finish Qt selection and retained payload integration for the NTFS
keep-loader/SBR route. Freeze the selected cache/download or original embedded
core before authorization, preserve Yes/No/Cancel and Syslinux-suppressed lookup,
and qualify transitions plus one complete simulated USB helper/READY job before
admitting it. FAT32 and patch/masquerade SBR sequencing, GRUB4DOS SBR, localized
no-BOM SIF, compatible FAT16, nonconverging roots and Windows To Go remain required.


## CP22n37 — Qt Windows NTFS with an independent frozen GRUB2 SBR (2026-10-04)

Enable the qualified NTFS/Alt+E WinPE keep-loader route through Qt. Reuse the
existing source preflight, original GRUB version lookup, cache/signed download,
Yes/No/Cancel, confirmation and one authorized helper. Keep original Windows
MBR/PBR selection independent of the shared GRUB2 SBR choice. No new operation,
protocol field, installer flag or compatibility abstraction. Only NTFS is offered
for this route; FAT32 and patch/masquerade SBR sequences still need qualification.

Move the helper's original embedded-core reader into the existing shared core
source library and move its original resource registration there. Qt and helper
now read the same resource through one implementation. Cache/download results
already own their bytes; No, unusable-cache and matching embedded-version choices
freeze the original embedded bytes before confirmation. Existing process staging
supplies path/size/hash after confirmation; the helper retains/checks the snapshot
before target access. Later cache changes cannot replace the accepted payload.
Use shared SBR dispatch when passing bytes: a Syslinux-suppressed lookup still
selects no core for installation. Original Windows/shared algorithms, payload,
writer and protocol remain unchanged. Eight of the previous 694 production files
change, 686 hashes match, and one resource file is added; CMake also changes.

Forty-two new Widgets cases across ISO/UDF and both Windows MBR choices cover
No/cache/signed download, Cancel, signature refusal, source mutation, unusable
cache, matching embedded version, unknown provider, partial-minint families and
Alt+E/NTFS/hybrid transitions. Preserve unknown metadata. Original scanning drops
an unrecognized directory-only GRUB2 provider; the accompanying GRUB4DOS then
wins SBR dispatch and remains gated. The test must expect that original result,
not invent GRUB2 or a version. A privately linked unchanged Qt selector refuses
all four new ISO/UDF/MBR positives while existing plain minint creation passes.

Normal and ASan/UBSan/leak runs pass 269 focused Widgets, 1,114 source, 73 request,
49 core-cache/resource and 309 native-helper cases. Embedded-core checks compare
the original bytes/hash and exact gap boundaries, clearing output on refusal.
Retain initial fixture failures: omitted GRUB copy denominator, absent cache
parent, cache leaking between rows and the incorrect unknown-provider expectation.
Correct only those fixtures/expectations; no production or sanitizer gate weakens.

One complete minint/both-GRUB/Win7-MBR NTFS Widgets job passes on a simulated USB
with the preferred kernel ntfs driver. Answer the original version prompt No;
freeze the original 42,742-byte core. Independently compare its exact placement
at byte 512 and the zero remainder of the gap, original Win7 MBR/active 0x80/NTFS
records, clean filesystem and all eleven prepared target files. Preserve the
existing loader/differing root detector/root SIF and patch only grub.cfg's label.
One simulated authorization, held-exit Finalizing below full progress, synced/
unmounted/released helper exit, full READY and unplug preservation pass. The
original generated BIOS loader executes with drive 0x80 and an unchanged disk
snapshot. This qualifies creation and that generated handoff; it does not prove
actual Windows Setup/detector or execution/ABI of the unused GRUB core.

The initial guest models UID 1000 through a root GUI driver but fails to change
ownership of this new staged payload. Extend only its existing guest launcher
ownership setup to WinPE snapshots; keep the production ownership check intact.
Retain failed guest evidence. Six invalid new SBR recipe combinations refuse
before build/output access. Host polkit, physical USB, NTFS-3G execution for this
mixture and another Windows customization matrix are not newly qualified.
Compact baseline/after/diff, manifests, controls, build/test logs, failed/successful
guest evidence and reproduction:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n37`.
Verify evidence then remove disposable builds/guest/control files; preserve ISO/deps.

Next CP22n38: connect the same independent SBR to advanced FAT32 keep-loader
preparation. Reuse the selected snapshot and existing core/PBR/WinPE operations;
audit fresh FAT32 root-loader restrictions and ordering before admission, then
qualify a coherent route. Patch/masquerade SBR, GRUB4DOS SBR, localized no-BOM SIF,
compatible FAT16, nonconverging roots and Windows To Go remain required work.


## CP22n38 — advanced FAT32 WinPE with the same independent GRUB2 SBR (2026-10-04)

Admit the Alt+E/shared keep-loader WinPE route on FAT32 as well as NTFS. Reuse the
existing selected core snapshot, process staging, fresh source/payload preflight,
partition-gap writer and WinPE finalization. Four production files change; the
other 691 of 695 production hashes match. No new production file, operation,
protocol field, installer flag, boot payload or compatibility layer. CMake only
adds a test fault wrapper. Original Windows/shared policy and writers are unchanged.

The FAT helper now loads/installs a selected independent core just as the NTFS
helper does. Keep the Windows MBR independent, install the core before mounting,
copy/prepare files, sync/private-unmount, then install the existing final Windows
FAT32 records with each copy's allocation hints. This keep-loader route retains
active/drive 0x80; no masquerade or final MBR rewrite. Preserve original lookup,
Yes/No/Cancel, immutable cache/download/embedded bytes and one authorization.
Qt uses the normal Windows FAT32/NTFS choices and shared default rules again;
Alt+E before loading these small images defaults to FAT32, while an explicit
NTFS choice survives refresh. FAT16, patch/masquerade and GRUB4DOS SBR stay gated.

Audit the actual fresh-source rule: competing root NTLDR, IO.SYS and MSDOS.SYS
refuse FAT32 before lookup/confirmation/authorization. A supported root BOOTMGR
is allowed and retained under the existing keep-loader rule. Test those facts
across ISO/UDF/both Windows MBRs and preserve missing/options/unknown/provider,
customization, EFI, pending, mutation, read-error and cancellation refusals.
Do not infer a version or relax collisions to make this route pass.

Normal and ASan/UBSan/leak runs pass 1,246 source, 74 request, 333 native-helper
and 315 focused Widgets cases. Add 132 source, one request, 24 helper and 46 Qt
rows using the existing fixtures. Readback checks original MBR/active/type/core/
gap and both final FAT boot-record copies, preserving their pre-write FSInfo
hints. Verify core-before-mount and final-records-after-finish ordering, failed
copy/sync/cleanup preventing final records, late primary/backup write/sync/readback
failures and cancellation. A private previous Qt selector fails all four new
FAT32 ISO/UDF/MBR positives; existing plain minint creation passes. Six invalid
SBR recipe combinations refuse before build/output access.

One complete minint/both-GRUB/Win7-MBR FAT32 Qt job passes on a simulated USB with
vfat. Original prompt No freezes the 42,742-byte embedded core. Independently
compare its placement at byte 512 and zero remaining gap, Windows MBR/active 0x80,
Windows FAT32/BOOTMGR records and backups, all eleven target files and clean
filesystem. Keep existing root BOOTMGR/differing detector/root SIF; only grub.cfg's
label changes. Primary FSInfo free clusters agree with the allocated FAT; the
backup preserves its original hint. One simulated authorization, held-exit
Finalizing below full, sync/unmount/helper exit, full READY and unplug preservation
pass. The generated BIOS loader runs with drive 0x80 and unchanged disk snapshot.
This does not qualify actual Windows Setup/detector, GRUB core execution/ABI,
host polkit or a physical USB, and adds no customization qualification.

Retain initial test failures: source policy expects the original pre-copy NT
record; final BOOTMGR code spans only part of the eight FSInfo hint bytes, so
compare the executable bytes plus the intersecting preserved metadata. Correct
only those readback expectations, with no production changes or weaker error/
sanitizer gates. Retain the initial recipe refusal for unrelated NTFS package
arguments; remove those arguments for the FAT32 run. Compact baseline/after/diff,
hashes, controls, build/test and guest readback evidence:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n38`.
Export/verify evidence before removing disposable builds/media; preserve ISO/deps.

Next CP22n39: audit and connect patch/masquerade WinPE with an independent GRUB2
SBR. Preserve the core/gap through final original Rufus MBR/drive-0x81 rewriting,
retain fresh loader/SIF/collision checks and normal 0x80 while Linux mounts, then
qualify one coherent route before admission. GRUB4DOS SBR, localized no-BOM SIF,
compatible FAT16, nonconverging roots and Windows To Go remain required work.


## CP22n39 — shared WinPE patch/masquerade with an independent GRUB2 SBR (2026-10-04)

Admit supported patch-loader WinPE with Alt+E and a selected GRUB2 SBR. Remove
only the keep-loader restriction from the existing selected-SBR classifier;
shared supported-layout checks still reject unsupported/partial setup families.
One production file changes, 694 of 695 hashes match. No new production file,
operation, protocol field, installer, payload, compatibility layer or helper code.
Original Windows/shared policies, transformations, native writers, staging and
Qt lookup/authorization/completion operations remain unchanged. CMake only adds
an existing-operation test wrapper for final MBR faults.

Reuse the complete existing sequence: fresh source/core before target access,
normal Windows MBR/active 0x80, native format/PBR/core, private mount/copy, shared
loader/SIF preparation, sync/private-unmount, final FAT32 records when applicable,
and original Rufus MBR/active 0x81. libfdisk wipe is disabled and the existing
final MBR rewrite preserves the independent core and remaining partition gap;
verify exact bytes rather than assume it. Keep both selected initial Windows MBR
choices; the shared masquerade plan forces the final Rufus MBR. Do not rescan or
mount after publication of 0x81. PATCH with /minint remains supported through the
same source/setup plan but does not masquerade or copy a root SIF; test it too.

Normal and ASan/UBSan/leak checks pass 1,318 source, 74 request, 379 native-helper
and 347 focused Widgets cases. Extend existing fixtures with 72 source and 32 Qt
rows; add 48 helper rows and replace two former patch refusals. Cover i386/AMD64,
ISO/UDF, NTFS/FAT32, both initial Windows MBR choices, original CRC/path/rdisk
changes, prepatched root convergence, root SIF editing, /minint without a minint
directory, short/malformed/partial/mismatching source and before-target refusal.
Keep cache/download/embedded freeze and Cancel/signature/source-change regressions.

New helper cases independently compare original final MBR/flag/core/zero gap,
loader/SIF output, both FAT32 boot copies and preserved allocation hints. Assert
active 0x80 while mounting and sync/private-finish before final FAT records/MBR.
A prior copy/sync/cleanup failure prevents final publication. Inject final MBR
operation/write/sync/readback failure and partial-write cancellation through the
existing writer; propagate failure with no successful completion. Final /minint
cases retain active/drive 0x80. A privately linked previous classifier refuses
eight new patch family/ISO/UDF/filesystem positives while keep-loader creation
passes. Six invalid SBR recipe combinations refuse before build/output access.

One complete generated AMD64/both-GRUB/Win7-initial-MBR FAT32 Widgets USB job passes
with vfat. Original lookup No freezes the original 42,742-byte embedded core.
Retain the existing prepatched root loader and differing detector; edit the
existing root SIF with the original source-device line. Independent readback
checks final Rufus MBR/active 0x81, original Windows FAT32/BOOTMGR records with drive
0x81 and backups, exact core at byte 512/zero remainder, clean volume/allocation
hints and eleven target files. Loader paths select AMD64; CRC and rdisk updates
match the original shared pass. Only the root SIF and grub.cfg label change
relative to this prepatched source; no source file is modified.

One simulated authorization, held-exit Finalizing below full, sync/unmount/helper
exit, full READY and unplug preservation pass. The generated BIOS loader runs
with drive 0x81 and unchanged disk snapshot. This qualifies creation and that
handoff, not actual Windows Setup/detector, execution/ABI of the unused GRUB core,
host polkit or physical USB. No additional Windows customization qualification.

Retain initial test/control failures. A readable SIF with no OsLoadOptions is
known "/minint not requested" in the original scanner; correct that new test
expectation rather than production behavior. The first private-control script
assumes incorrect filesystem enum values, generating nonexistent rows; resolve
actual QTest tags and require all eight named failures with no missing-tag error.
Final controls use the final driver. Compact baseline/after/diff, hashes, controls,
build/test and simulated USB/readback evidence:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n39`.
Export/verify evidence before removing disposable builds/media; preserve ISO/deps.

Next CP22n40: investigate the original GRUB4DOS SBR provider and reuse its payload,
shared precedence and bounded gap writer where possible. Keep unknown metadata
unknown and qualify a coherent native route before admission. Localized no-BOM
SIF, compatible FAT16, nonconverging roots and Windows To Go remain required work.

## CP22n40 — original GRUB4DOS boot records through the existing gap writer (2026-10-04)

Audit original MBR/SBR, standalone download/copy and image-loader discovery.
Reuse the original 423-byte ms-sys MBR array and the bundled 8,192-byte 0.4.6a
resource. Skip its first sector and place exactly 7,680 remaining bytes at byte
512, as Windows WriteSBR. The resource prefix equals the original MBR array,
but must not replace the independently selected bootstrap/signature/table.
No external/native installer is needed for those boot-record operations.

Extend only the existing partition adapter and its header: add the GRUB4DOS MBR
choice and a resource-oriented SBR entry point sharing the existing bounded I/O
implementation with GRUB2. Preserve the public GRUB2 API and original Windows
code/resources/shared selection policy. Two production files change; 693 of 695
production hashes match. No new production file, dependency, helper operation,
protocol field, download path or GUI option. Accept FAT16/FAT32/NTFS with original
GRUB4DOS MBR; reject exFAT/ext. Allow an independent SBR with the existing original
Windows/Rufus/masqueraded MBR, reject Syslinux/zero and the other GRUB provider's
MBR. Low-level acceptance does not enable or qualify an arbitrary image route.

Normal and ASan/UBSan with leak detection pass 700 GRUB4DOS SBR cases, 658 GRUB2
regressions, 279 GRUB4DOS MBR creation/readback calls, 89 preparation/readback calls
and the existing GRUB2 ext2/ext3 layout suite. Full Qt GUI/helper builds pass.
Reuse the existing functional/fault drivers rather than duplicate them. Exact
readback checks the original SBR and all surrounding bytes, seeded unused MBR
bootstrap bytes, signature/table, resource immutability, proper header skipping,
minimum/full-gap bounds, cancellation, short/interrupted/failed I/O, flush/readback
faults, changed target facts, provider choice and retained descriptor state.
A final original WinPE masquerade rewrite preserves the independent SBR on all
three supported filesystems. No READY or privilege behavior is changed.

Previous adapter controls refuse three new GRUB4DOS positives without mutation,
while three original GRUB2 controls pass. A private mutation including the first
resource sector is detected by the final SBR test. No production binary is
replaced by a control. The first normal run passes with 658 GRUB4DOS cases;
subsequent test-only additions extend it to 700 and only the two SBR suites are
rerun. Final sanitizer runs use all final cases. Keep separate captured logs for
these runs rather than rely on overwritten CTest last-run metadata.

This qualifies regular-file adapter operations, not execution of GRUB4DOS, a
complete helper/GUI job or physical USB. Image scanning detects root grldr without
establishing a version; preserve that unknown. Standalone Windows obtains grldr
from its versioned signed-download cache, then copies it to the target. Integration
must retain a coherent loader/provider contract and fail a missing/failed required
copy rather than publish success. Audit/reuse detail:
[GRUB4DOS reuse](linux-grub4dos-reuse.md). Compact baselines, diffs, immutable payload
hashes, controls and build/test evidence:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n40`.
Export/verify before removing disposable builds/media; preserve source ISO/deps.

Next CP22n41: connect the bundled GRUB4DOS SBR to the existing advanced WinPE
helper flow with fresh-source/resource checks and original precedence. Qualify
one coherent creation route before Qt admission. Keep unknown image versions
unknown; standalone signed grldr download/copy and actual GRUB4DOS execution
remain separate required work. Localized no-BOM SIF, compatible FAT16,
nonconverging roots and Windows To Go remain required work.

## CP22n41 — bundled GRUB4DOS SBR in the existing WinPE helper (2026-10-04)

Connect the original GRUB4DOS SBR to advanced legacy WinPE FAT32/NTFS jobs.
Use the freshly checked report and shared GetSecondaryBootType to select it;
Syslinux suppresses both GRUB providers and selected GRUB2 retains precedence.
Load the complete immutable bundled resource and check its embedding gap before
opening/unmounting the target. Retain it in the existing worker-owned payload;
never infer an image grldr version or reopen a cache/download path for this SBR.
An incompatible explicit GRUB2 core remains rejected before target access.

Extend the existing resource collection/reader and helper load/install operations
rather than add a provider library, download path, request field, helper operation
or compatibility framework. Six production files change, 689/695 hashes match;
no new production file. Original Windows/shared policies, writers and resource
bytes are unchanged. Keep a separate GRUB4DOS candidate opt-in, default false,
so Qt admission remains gated while native preflight/helper qualification proceeds.
No standalone GRUB4DOS or general GRUB4DOS ISO route is enabled by this change.

Reuse the complete existing helper flow: normal Windows MBR/active 0x80, native
format and independent SBR, Windows partition boot records, private mount/copy,
original shared WinPE loader/SIF preparation, sync/private finish and final FAT
records/masqueraded MBR where needed. The final 0x81 rewrite preserves the exact
SBR and zero remainder. /minint patch jobs retain 0x80. Image grldr is copied
unchanged through ordinary ISO extraction; opaque content remains unversioned.
The WinPE boot chain executes the selected Windows loader rather than this SBR.

Normal and ASan/UBSan with leak detection pass 1,366 source, 419 helper, 50
resource/cache and 74 request cases. Add 48 source and 40 helper rows and one
resource reader case to existing drivers. Cover ISO/UDF, both original Windows
MBRs, FAT32/NTFS, keep-loader/i386/AMD64 patch/minint patch, no Alt+E, incompatible
core/UEFI request, precedence, resource gap limits, exact loader/SIF/core/gap/FAT
record readback, cancellation and resource/format/copy/sync/cleanup/final-MBR
faults. Resource failure stops before target access; prior failures prevent final
publication. Existing GRUB2 and source-validation regressions remain covered.
Full Qt GUI/helper builds and 116 focused normal Widgets cases pass. Those Qt
cases confirm its existing gates and GRUB2/WinPE behavior; they do not admit a
GRUB4DOS SBR job. No new Qt code or privilege/completion behavior.

One complete AMD64/GRUB4DOS/Win7-initial-MBR FAT32 simulated USB helper job passes
with vfat. Preserve existing prepatched root BOOTMGR and differing detector,
edit root SIF through the shared implementation, copy all nine expected files
including grldr/menu.lst and retain only that root-SIF difference from source.
Independent readback verifies original 7,680-byte SBR SHA256 at byte 512, zero gap,
final Rufus MBR/0x81, original FAT32 boot records/backups/allocation hints and clean
filesystem. One simulated authorization, sync/private cleanup/helper exit before
completion pass. Generated BIOS loader runs with drive 0x81 and unchanged disk
snapshot. No actual Windows Setup/detector/GRUB4DOS execution, host polkit,
physical USB, Qt READY or additional Windows customization qualification.

A private previous-admission gate rejects eight named new keep/patch positives
while an existing GRUB2 control passes. Four invalid VM combinations refuse
before build/output access, including the still-gated Qt GRUB4DOS route. Preserve
initial source-test failures: four former GRUB4DOS refusals are now supported;
four invalid FAT32/UEFI requests return protocol EINVAL before image ENOTSUP.
Update these expectations, not production validation. Normal cache/request/helper
suites already pass; rerun only source after that test-only correction. Final
sanitizer coverage uses all final cases. The first VM command refuses for an
unbuilt udisks_fixture; build that guest target and the same qualification passes.

Compact baselines/diff, production/source/binary hashes, controls, normal/sanitizer
and Qt logs, generated guest/readback/BIOS evidence:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n41`.
Export/verify before removing disposable builds/media; preserve real ISO/deps.
See [the GRUB4DOS reuse audit](linux-grub4dos-reuse.md).

Next CP22n42: enable and qualify the original Alt+E GRUB4DOS-SBR WinPE choice in
Qt, with ISO-load/selection refresh, existing confirmation/one authorization and
sync/cleanup/helper/launcher exit before full READY. Reuse the qualified bundled
resource flow; do not invent a download, image version or request payload for it.
Standalone signed grldr download/copy and actual GRUB4DOS execution, other GRUB4DOS
ISO routes, localized no-BOM SIF, compatible FAT16, nonconverging roots and Windows
To Go remain required work.


## CP22n42 — Qt WinPE selection with the original GRUB4DOS SBR (2026-10-04)

Enable the qualified advanced WinPE FAT32/NTFS route in the four existing Qt
candidate/admission checks. Reuse the CP22n41 helper/resource path unchanged.
One production file changes, four lines; 694/695 production hashes match the
previous checkpoint. No new production file, request field, GUI dialog, payload
selection/download path, dependency or helper operation. Original Windows code,
shared policies, bundled boot records and helper sequencing remain unchanged.

Alt+E before ISO load uses the shared FAT32 default; enabling it after load
retains the current NTFS choice. Both filesystems remain available with the
original Windows/Rufus MBR choice, BIOS target and existing confirmation. Toggling
Alt+E off disables this ISO lane while preserving valid hybrid DD; restoring it
retains the user's filesystem. Known GRUB2 and Syslinux keep original precedence.
An unknown directory-only GRUB2 version stays unknown: original scanning removes
that provider, and remaining root grldr selects the bundled GRUB4DOS SBR without
a GRUB2 download prompt or owned/staged GRUB2 core. This does not establish the
image grldr version or qualify general GRUB4DOS image booting.

Normal and ASan/UBSan with leak detection pass 306 focused Widgets cases. Add 52
rows: 32 keep/i386/AMD64/minint-option creation choices across ISO/UDF, FAT32/NTFS
and Win7/Rufus MBR; 12 confirmation-cancel, source-change, sync/cleanup failure,
held-result and bootloader-cancel cases; eight ISO/hybrid selection refresh cases.
Update eight existing unknown-version cases to verify the original GRUB4DOS
selection without inventing a GRUB2 version. Existing 112 GRUB2-flow rows,
136 plain/mixed WinPE rows and refresh/prompt regressions pass. Full GUI/helper
and guest drivers build. Backend/component suites from CP22n41 are unchanged.

One complete AMD64 FAT32 simulated USB creation through Qt passes with initial
Win7 MBR, existing prepatched root BOOTMGR, differing detector and root SIF.
Verify original SBR bytes at byte 512, zero unused embedding gap, final Rufus
masquerade MBR/active 0x81, Windows FAT32 records and primary/backup FSInfo hints.
Nine target files match independent expectations; only root TxTsEtUp.SiF differs
from the source. grldr/menu.lst, detector and existing patched loader are unchanged.
The generated BIOS Windows loader boots with drive 0x81; its snapshot is unchanged.

The guest asserts existing confirmation before one simulated authorization,
no GRUB2 payload/download or Syslinux install, copy-byte denominator, successful
sync/private cleanup, held launcher exit preventing full READY, full READY after
exit/claim release and preserved READY on unplug. No actual Windows Setup,
GRUB4DOS execution, physical USB, host polkit or additional Windows customization
qualification is claimed. Kernel 7.2.8-arch1-2 and TCG are used without host root.

A private previous-selector executable fails eight named new positives and
passes an existing GRUB2 control. Four invalid VM recipes refuse before build or
output creation. The first private CLI-control invocation omitted the required
kernel argument; preserve that diagnostic and correct the command, without any
product change. All final GUI/component controls and complete creation pass.
Compact baselines/diff, source/production/binary hashes, normal/sanitizer/control
logs, recipe and independent guest/readback/BIOS evidence:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n42`.
Export/verify before cleaning all disposable checkpoint builds/media; preserve
real source ISO and reusable dependencies. See [reuse audit](linux-grub4dos-reuse.md).

Next CP22n43: connect the standalone GRUB4DOS grldr requirement to the existing
versioned signed cache/download and authorized copy flow. Check the selected
loader and bundled records before target access, reuse shared standalone policy
and qualify a complete native FAT32 route before exposing it in Qt. Qualify actual
GRUB4DOS execution separately from this Windows loader handoff. A missing or failed
required loader must prevent READY; document this justified difference from the
Windows branch's logged copy failure. Other compatible GRUB4DOS image routes,
localized no-BOM SIF, compatible FAT16, nonconverging roots and Windows To Go remain
required work. Keep the one-customization qualification limit.


## CP22n43 — standalone GRUB4DOS loader selection through existing cache/download code (2026-10-04)

Split the next standalone route at a useful boundary: qualify unprivileged grldr
selection first, then connect its frozen payload to the authorized copy/helper
route and qualify a native FAT32 job before exposing standalone Qt selection.
Do not combine downloader, protocol, formatting and boot changes in one checkpoint.
The CP22n42 WinPE SBR route remains unchanged and qualified.

Extend existing grub2_cache and Grub2CoreSelection rather than add another cache,
downloader, provider library or production file. Reuse one bounded source reader,
identity checks, owned byte/hash snapshot, atomic QSaveFile replacement and existing
SignedDownload transport/key verification. Four production files change;
691/695 production hashes match CP22n42. CMake changes only a test fixture path.
Windows code/resources, shared policy, helper/request/process and GUI admission
remain unchanged. No device access, authorization or helper launch in selection.

Use original GRUB4DOS_VERSION and fixed grub4dos-0.4.6a/grldr path/URL. No ISO report,
host/version inference, shortened-name retry, GRUB2 core substitution or embedded
grldr fallback. Existing memory bound is 1 MiB minus 512; it limits file buffering,
not a filesystem loader's embedding gap. The cache reader requires a regular file
of at least 512 bytes, detects replacement/change/removal around bounded reads and
retains accepted bytes independently of later path changes. Stable ordinary-user
cache symlinks retain original lookup behavior; helper input policy remains a
separate future boundary. Cache reuse itself does not verify authenticity, as on
Windows and the existing GRUB2 path; new downloads verify the original detached
signature before atomic storage and independent readback. A substituted cache at
readback returns ESTALE; result bytes come from the verified snapshot.

A required loader cannot fall back to an empty successful result. No returns
ENOENT and Cancel ECANCELED; malformed cache, download/signature/save/readback
failures stop selection. This intentionally differs from Windows's No path,
which proceeds and merely logs a failed grldr copy later: successful bootable
creation without its required loader would mislead the user. Selection will stop
before authorization or target writes. A later copy failure must likewise prevent
successful READY. Preserve original Yes/No/Cancel prompt wording at Qt integration;
this checkpoint adds no actual standalone GUI dialog/choice.

Fetch the original Rufus server grldr/signature once and keep compact offline test
fixtures, without a test signing key or trust override. The 322,965-byte payload
contains the 0.4.6a 2024-02-26 banner matching the bundled resource's documented
release; SHA256 19ba57329be745ceae46354c5d411b7d8efec6802cfd7f6fee986df4b763f6fe.
Its 256-byte detached signature verifies with the original Rufus public key;
SHA256 698052e99cf1b92924b47485ee94b616a67e3c855a9c2eb4e777de64f729ffee.
Record URLs/lengths/hashes/provenance in tests/fixtures/download/grub4dos-0.4.6a.
This fixture identity is not a general ISO grldr version or boot-compatibility claim.

Normal and ASan/UBSan with leak detection pass 80 reader, 75 atomic-storage,
88 selection and 63 signed-transport cases (306 total). Add 95 loader cases using
the existing fault drivers and private loopback HTTP fixture. Cover fixed path,
missing/unsigned/malformed cache, bounds/nonregular inputs, stable and retargeted
symlinks, short/interrupted/failed I/O, identity mutations, cancellation,
atomic replacement failures/old-file preservation, real signed download, missing
payload/signature, rejected signature/changed payload, oversized download,
save/readback/substitution errors and no prefix fallback. Verify synchronous prompt
cancellation, receiver deletion, destructor abort, reentrant completion/restart and
GRUB2 -> GRUB4DOS -> GRUB2 reuse with restored ISO checks and a final GRUB2 download.

Full GUI/helper builds and 159 focused normal Widgets GRUB2/WinPE/GRUB4DOS/prompt
regressions pass. Backend/protocol/GUI admission are unchanged; no new USB/VM or
Windows customization qualification is needed for this loader-selection boundary.
Two private mutations allowing an empty success after No or rejected download are
detected by final tests, while existing GRUB2 fallback/signature controls pass.
Initial normal/sanitizer component runs pass with 87 selection cases; one test-only
reentrancy addition and stronger provider-switch check bring final selection to 88.
Rerun only the changed selection suite, retaining earlier passing logs.

Compact baseline/diff, production/source/binary hashes, tests/mutation controls,
fixture provenance and reproduction recipe:
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n43`.
Export/verify and clean disposable builds/download copies; preserve real ISO,
reusable dependencies and repository's compact offline test fixtures.
See [the GRUB4DOS reuse audit](linux-grub4dos-reuse.md).

Next CP22n44: reuse frozen-file process staging/metadata and bounded source checks
for the required standalone loader, then connect it to the existing authorized
FAT32 copy/sync/cleanup and original GRUB4DOS MBR/SBR flow. Check loader/resource
before target access; missing/failed copy must stop READY. Qualify a complete
native FAT32 simulated USB/readback route before standalone Qt exposure. Actual
GRUB4DOS execution, FAT16/NTFS and general GRUB4DOS ISO routes remain required.
Do not overwrite an image's unversioned grldr with the standalone loader. Other
legacy/Windows To Go work and the one-customization qualification limit remain.


## CP22n44 — standalone GRUB4DOS frozen loader and native FAT32 helper (2026-10-05)

Connect CP22n43's accepted owned loader bytes to the existing process/helper path.
Nine existing production files change; no new production file, library, downloader,
privilege mechanism or disk writer. Reuse private 0700 staging / 0400 input,
existing bounded path/size/SHA256 metadata and source identity reader. The explicit
prepare-grub4dos-fat32-device-v1 operation requires a complete loader fingerprint,
MBR/FAT32 and no ISO, other boot provider, UEFI layout, DOS or Windows settings.
Reject missing/invalid input before launching authorization. Keep the existing
GRUB2 metadata/API names for compatibility, documenting that this operation carries
filesystem grldr rather than embedded core.img. No image-version inference.

The helper checks caller ownership, regular single-link input, readable permissions,
exact size/hash and identity around bounded reads, then owns the accepted bytes.
Reuse the GRUB2 helper reader; keep its embedding-gap gate on the GRUB2 entry point
only. The standalone filesystem loader uses the existing memory bound, not the gap.
Reject a GRUB4DOS request at the GRUB2 loader entry point. Read/check the original
bundled GRUB4DOS SBR before target access, retaining it separately from grldr.
Never reopen the staging/cache path after preflight or replace an image's grldr.

Feed BT_GRUB4DOS into shared GetMasterBootRecord, install original MBR/SBR and
standard FAT records through existing writers, then mount and copy root grldr.
Reuse the existing FreeDOS module's exclusive bounded file writer/readback and
file/directory fsync, without its DOS attributes, resources, startup or progress
milestones. Reuse finishCopy for filesystem/device sync and private-mount cleanup.
Missing or failed loader copy prevents successful READY, unlike Windows's logged
CopyFile failure; a bootable success without its required loader would mislead users.
Process success additionally requires exactly the frozen loader byte count, all
phases, clean sync/cleanup and actual launcher exit. Staging survives through exit.
One existing authorized helper handles the complete job. No standalone Qt exposure
or new host detection/customization test in this checkpoint.

Normal and ASan/UBSan with leak detection pass 80 retained-source, 209 request/process,
33 GRUB4DOS copy, 31 FreeDOS copy, 64 FreeDOS startup and 16 focused standalone helper
cases (433 total). Source faults cover ownership/permissions/links/nonregular input,
bounds, hash/size change, short/interrupted/failed reads, cancellation and replacement.
Copy faults cover exclusive creation, nonempty/raced targets, partial/failed/zero
writes, corrupt/failed readback, cancellation, close and file/directory sync failures.
Request checks refuse absent/malformed loader metadata and incompatible options;
process checks reject incomplete copy, skipped phases, sync/cleanup errors and wrong
exit, hold staging until child exit and launch authorization once. Full-helper cases
check failure before target/writes, original records, loader copy, retained bytes
through a changed staging path, formatting/boot/mount/copy/sync/cleanup failures.
Existing source-backed helper regression suite passes 419 cases and focused Widgets
GRUB2/WinPE/GRUB4DOS/prompt regressions pass 159 normal cases. A private mutation
accepting a zero-byte GRUB4DOS success is caught; the GRUB2 control still passes.

The existing QEMU simulated-USB recipe qualifies one complete production FAT32 job
on kernel 7.2.8-arch1-2 with real block claims, formatter and kernel FAT mount. Use
one guest-only simulated authorization; hold its launcher after the real helper
exits to verify no premature completion. Verify private-mount/staging cleanup,
descriptor/descendant release and exclusive reopen after completion. Independently
read original MBR bytes, 7680-byte SBR, zeroed remaining gap, primary/backup FAT
records, extent/serial, filesystem consistency and exactly one root grldr matching
the 322,965-byte original fixture. This qualifies creation/readback, not GRUB4DOS
execution or a real USB/host polkit agent. Keep standalone FAT16/NTFS and general
GRUB4DOS image routes required. No extra Windows customization qualification.

Initial test integration failures were missing test fixture/oracle build definitions
and an incorrect test-method invocation. The first native driver also lacked its
usual /dev/block link in the minimal guest without udev, and observed the held-exit
marker after processing completion. Fix those fixture/driver mistakes; keep all
production guards and final expectations. The complete existing helper regression
suite and final native/readback run pass without production changes for those issues.

Evidence, baseline/diff, source/production/binary hashes, logs, private control and
reproduction recipe: /home/marius/proj/lufus/.rufus-test-artifacts/cp22n44.
Export/verify compact evidence before deleting disposable builds and guest media.
Preserve the real Windows ISO, reusable dependencies and compact offline fixtures.

Next CP22n45: qualify actual BIOS execution of the helper-created original GRUB4DOS
loader, then connect the existing acquisition provider and frozen payload to the
standalone Qt choice in a separate small checkpoint. Preserve original prompt
wording, cancellation/no-before-authorization and completion/progress behavior.
FAT16/NTFS, general ISO GRUB4DOS routes, other legacy/Windows To Go and complete
Windows parity remain required; keep the one-customization qualification limit.


## CP22n45 — actual standalone GRUB4DOS BIOS execution (2026-10-05)

Qualify the CP22n44 helper-created FAT32 stick through its own original MBR/SBR,
filesystem grldr and BIOS USB path. No production change, alternative payload,
menu/config injection, synthetic second-stage loader or direct Linux kernel/initrd
in the boot VM. All 695 production hashes remain unchanged. Recreate the complete
native helper job with its existing QEMU USB/kernel FAT fixture and retain all
CP22n44 single-authorization, sync/cleanup/held-exit and independent byte readback
checks before attempting boot. Keep standalone GUI exposure in the next checkpoint.

Extend the existing --grub4dos-helper recipe with a small test-only boot module.
Reuse existing BIOS firmware recording, QMP keyboard/VGA readers and screen waiter;
generalize only that waiter's failure message from FreeDOS to BIOS. Boot snapshot
media with SeaBIOS and USB mass storage; record firmware/QEMU identity, commands,
screenshots, VGA text and disk hashes. The loader's original preset menu naturally
falls through its menu.lst search to the command prompt; do not add a menu.lst.
Require its original 0.4.6a 2024-02-26 banner and grub> prompt, then execute root,
ls /, crc32 /grldr and echo RUFUSGRUB4DOSOK. Match distinct command responses and
the next prompt, not typed command echoes or stale startup text. Root must identify
(hd0,0), FAT32 and partition type 0x0C; directory listing must contain grldr. Its
own FAT reader must calculate loader CRC32 2b53370c, independently matching the host
CRC of the unchanged pinned payload, and the marker must appear on its own line.
The backing disk hash must remain unchanged after snapshot boot and clean VM exit.
CRC command syntax follows the [upstream built-in definitions](https://github.com/chenall/grub4dos/blob/0.4.6a/stage2/builtins.c).

A private copy with only grldr removed must fail the same boot qualifier before
any command checks. Require the original "Cannot find GRLDR in all drives." error,
not just a timeout or broken QMP/firmware launch, and unchanged negative backing
image after snapshot execution. Delete both temporary test media. This control
shows the qualifier detects the missing-loader defect that Linux now prevents from
being reported as successful creation. It is not a relaxation of request/preflight.

The complete native creation/readback, BIOS command/CRC checks and negative control
pass under TCG. Strengthen the first passing test to require exact root/filesystem
and the original missing-loader diagnostic; rerun the final complete route. The
production helper, boot records, source/copy validation and Qt admission are unchanged.
No sanitizer rebuild/matrix is needed for this test-only checkpoint; CP22n44's
production sanitizer and regression evidence remains applicable. No additional
Windows customization, physical USB, host polkit, general ISO, FAT16/NTFS or OS
chainload/Windows Setup qualification is claimed. This establishes execution of
the fixed original standalone GRUB4DOS loader, not compatibility of an image's grldr.

Evidence: /home/marius/proj/lufus/.rufus-test-artifacts/cp22n45. Export/verify compact
baseline/diff, production/source/binary/firmware hashes, native creation/readback,
positive/negative VGA/screenshot/command evidence and reproduction, then clean the
completed builds/guest media. Preserve source ISOs and reusable dependencies.

Next CP22n46: expose the qualified standalone FAT32 GRUB4DOS route in Qt using the
existing acquisition provider and frozen payload. Preserve original selection and
Yes/No/Cancel prompt wording; No/Cancel and source/cache/download failures precede
authorization. Match standalone progress/completion using Rufus's shared policy,
including full READY only after sync/cleanup/helper/launcher exit and stable unplug.
Keep other filesystems, general GRUB4DOS image routes, remaining legacy/Windows To Go
and complete Windows parity required. Keep the one-customization qualification limit.


### CP22n46 — standalone GRUB4DOS FAT32 Qt integration (2026-10-05)

Expose the already-qualified fixed 0.4.6a loader through the normal advanced
Widgets boot choice. Preserve SetBootOptions ordering: Grub 2.14, Grub4DOS 0.4.6a,
UEFI:NTFS. Select MBR and BIOS (or UEFI-CSM); only qualified FAT32 is offered.
Preserve the retained ISO when choosing standalone boot and restore image selection
when hiding advanced choices. Do not use an image's unversioned grldr or infer a
compatible installer from it. Other Windows-supported GRUB4DOS filesystems remain
required, not silently admitted through the new combo entry.

Reuse Grub2CoreSelection::startGrub4dos, original-key signed transport/cache,
existing selection state/dialog/cancellation, strict request protocol, private
staging and single complete-job helper. No new production file, downloader, GUI
bridge, progress algorithm or privilege flow. Four existing production files
change: format_options, selection_controller, iso_write_controller, main_window.
Original Windows/shared policy, backend helper and payloads remain unchanged.

The existing strict GRUB4DOS request requires loader metadata. Select/freeze the
loader before source-free preflight, as the existing FreeDOS settings chooser also
precedes checks. Both checks and download finish before confirmation/authorization;
repeat device and formatter checks after destructive confirmation. The controller
uses cache path/size/hash for strict preflight, then removes those path fields only
from its local launch copy so PrepareImageProcess creates the private 0400 grldr
from retained bytes. Keep its guard against overwriting supplied metadata. Changing
the cache after selection cannot substitute the loader. Initial integration tests
caught a controller handoff defect (passing already-filled metadata into the
stager); correct the controller without relaxing the launcher or request boundary.

Use original MSG_103 Download grldr? and MSG_104 explanation/Yes choice, with a
specific Linux adjustment to the No instruction and cache directory. Windows No
allows finishing without grldr for later manual copying. CP22n43/44 deliberately
stop before authorization instead of reporting bootable creation without its
required loader. The dialog therefore says No stops so the file can be supplied
manually before starting again, and retains Cancel as the default/escape choice.
No/Cancel, malformed cache, signature/download failures or device invalidation
must not authorize or alter the target. No empty-loader or embedded GRUB2 fallback.

Select BT_GRUB4DOS in the unchanged shared progress weighting; original standalone
grldr copying exposes no byte-progress/copy milestones. Show loader installation
and Copying: grldr rather than an ISO percentage. Validate exactly the frozen
loader's copied byte count, planned partition, sync/private-mount cleanup and
complete helper/launcher exit before full green READY and one notification.
Holding a successful result with the child still running stays Finalizing.
Unplugging after success retains READY without another notification. Cancellation,
short copy, wrong layout, omitted finalization and sync/cleanup/authorization errors
cannot become successful READY.

Qualification uses the existing private-bus Widgets functional harness and a
protocol-only child: real UI actions, signed fixture/cache, immutable-byte handoff,
confirmation, process lifetime, errors and cancellation, without physical disks or
host polkit. Normal production GUI/helper build and sanitizer/leak checks apply.
CP22n44/45 already qualify unchanged native creation and actual BIOS execution;
this checkpoint does not claim a new complete native GUI creation/boot run.
Evidence: /home/marius/proj/lufus/.rufus-test-artifacts/cp22n46. Preserve compact
baseline/diff/log/source/binary hashes, then remove completed disposable builds.

Next CP22n47: extend the existing native Widgets guest driver for standalone
GRUB4DOS FAT32. Use the original cached/signed fixture, actual helper and virtual
USB, one guest-only simulated authorization, independent exact media readback and
CP22n45's BIOS command/CRC qualifier. Hold launcher exit and verify Finalizing,
private mount/staged-file/descendant/claim cleanup, full READY and stable unplug.
Reuse existing test drivers; no new production implementation merely for testing.
Do not add Windows customization qualification. General GRUB4DOS image routes,
FAT16/NTFS, remaining legacy/Windows To Go and complete Windows parity remain required.

Final CP22n46 results: 20 new GRUB4DOS scenarios (22 QtTest totals including
setup/cleanup) pass normally and under ASan/UBSan with leak detection enabled.
The complete existing Widgets suite passes 1322 cases, with five pre-existing
conditional skips: optional real Windows/Alpine inputs and three unqualified
unknown-version Syslinux success routes. Format policy: 56 passes; window selection:
77 passes; existing GRUB download/image regression under sanitizers: 102 passes.
Final dialog/progress checks pass after readability and cache-path clarification.
The independent original FAT32 progress expectation is 60853/65535 after the first
finalization step (13 of 14 discrete slots), below READY while the child remains alive.
Four production files changed; the other 691 tracked production hashes, including
Windows/shared code and native helper/backend, are unchanged. No new production file.
Only checkpoint additions pass the whitespace check; an unrestricted git diff check
also reports existing CRLF whitespace in src/parser.c, outside this checkpoint.


### CP22n47 — standalone GRUB4DOS Qt/native USB/BIOS qualification (2026-10-05)

Complete CP22n46's Qt route with the existing native virtual USB and BIOS tests.
Extend test_grub2_widgets_guest, guest-mbr and mbr.py with --grub4dos-gui; add only
test-target completion notification wrappers in CMake. All 695 production hashes,
including Qt, helper, Windows/shared policy and original payloads, stay unchanged.
No new production or test module, privilege API or parallel downloader. The native
Qt driver uses the existing PID-1 child fixture and models caller UID 1000 with the
existing getuid wrapper; the guest-only launcher owns its private 0700 directory
and 0400 single-link loader as that caller before starting the unchanged helper.
This is a disposable QEMU test, not desktop polkit or an actual unprivileged-user
session. No host root, mounts, physical disks or Windows customization qualification.

Drive the actual advanced boot combo to Grub4DOS 0.4.6a, check only FAT32/MBR/BIOS,
select RUFUS_TEST and use original fixed cached loader bytes. START performs strict
native device/formatter preflight; confirmation precedes the one authorization.
Check the frozen original loader hash/size/origin and metadata. Replace the cache
after confirmation is presented: private staging and written media must still
contain the retained original loader. Let the real native helper partition, format,
write MBR/SBR/PBR, privately mount, copy, sync and clean up. Hold the launcher after
the helper has exited. The UI must stay Finalizing at original progress 60853/65535,
with no completion beep/alert, while its private loader remains caller-owned 0400
inside 0700. The helper mount directory is already gone. Release the launcher and
require exact complete FAT32 extent, clean result, exact 322965 copied bytes, all
helper/formatter descendants joined, no private mount or staging, and O_EXCL disk
claim available. Only then full READY with one notification. Unplug keeps READY
and notification count unchanged. The private service and bus are reaped too.

Reuse CP22n45's independent MBR/signature, original SBR and zero gap, standard FAT
boot/backup, extent/serial/label/fsck/root-listing and exact grldr readback. Qt
chooses a random FAT volume ID; compare the on-disk value with the frozen GUI
request reported by the guest instead of the helper recipe's fixed test serial.
Keep the old helper-only serial/label expectations. The first native UI job passes
but host readback expects the old helper label RUFUS TEST; fix only that oracle to
expect explicitly selected RUFUS_TEST. Do not relax filesystem-error diagnostics.
A syntax check also catches an over-broad scripted serial edit in the unrelated
FAT inspector; restore that inspector byte-for-byte before any native run.

Boot the created media directly through SeaBIOS USB, with no boot kernel/initrd or
added menu. Require original loader version, root (hd0,0)/FAT32/0x0C, grldr listing,
CRC32 2b53370c through its own FAT reader, executed ECHO marker, unchanged backing
hash and clean VM exit. A private copy missing only grldr must fail this same boot
qualifier with Cannot find GRLDR in all drives. Final whole route passes after
strengthening private ownership/directory and retaining exact volume-size guards.
Check the helper-only recipe after extending this common test runner. No new
production sanitizer matrix is warranted: production is unchanged and CP22n46's
normal/sanitized GUI/component gates still apply. Preserve binary/source/firmware
hashes, serial/commands/VGA/screenshots/readback and positive/negative results;
remove all completed guest media/builds after exporting verified compact evidence.
Evidence: /home/marius/proj/lufus/.rufus-test-artifacts/cp22n47.

Next CP22n48: qualify standalone GRUB4DOS FAT16 through the existing FAT formatter,
standard FAT boot adapter, root loader copy and original MBR/SBR. Shared Windows
policy already allows FAT16/FAT32/NTFS. Check geometry, frozen-loader/protocol
admission, native copy/cleanup and actual BIOS loader readback before widening Qt
filesystem choices. Keep NTFS, general GRUB4DOS images, other legacy features,
Windows To Go and full Windows parity required; unknown image versions remain
unknown. No additional Windows customization qualification.

Final CP22n47 results: complete Qt/native creation/readback/BIOS/missing-loader
qualification passes, including the stricter caller ownership, directory permissions
and exact complete-volume assertions. The helper-only native/readback/BIOS/control
recipe also passes unchanged expectations. Only existing test driver/runner/guest
script, test-target link flags and documentation change. All 695 production file
hashes remain unchanged. Python/shell syntax and scoped whitespace checks pass.


## CP22n48 — standalone GRUB4DOS FAT16 helper and BIOS (2026-10-05)

Complete the source-free MBR/FAT16 helper route using the existing formatter,
standard FAT boot adapter, root loader copy and original GRUB4DOS MBR/SBR. The
only production change is FAT16 admission in prepare_image_protocol.cpp: encode
prepare-grub4dos-fat16-device-v1 and decode that exact operation as FAT16. Retain
required private loader metadata, source-free/native/MBR checks and incompatible
mode refusals. No new production module, formatter, loader or privileged operation;
all other 694 production file hashes remain unchanged. Qt remains FAT32-only until
the next checkpoint qualifies its filesystem selection and complete GUI route.

Request/process tests cover both filesystems, exact round trips, missing fields,
unsupported modes/filesystems and missing required loader. Held exit/cancel,
short/extra/zero copy, omitted phases/results and sync/cleanup failures must never
produce success. FAT16 owned-loader retention and unsupported NTFS refusal precede
reads. Run both FAT variants through the existing helper fault matrix: altered
hash/size/missing input fail before opening the target; formatter, original SBR,
standard PBR, mount/copy, sync and cleanup faults retain their expected errors.
A cache/file replacement after format must still copy the retained original bytes.

Create native 64-MiB virtual USBs with 2-KiB FAT16 and 512-byte FAT32 clusters using
one guest-only authorization each. Require original boot records, SBR/gap bytes,
partition geometry/serial, exact 322965-byte root-only grldr, final filesystem sync,
private unmount, held-launcher exit, reaped descendants, removed staging and released
exclusive disk claim. Independent filesystem checks permit only the original
standard boot records' precise NO NAME/root-label diagnostic. Bare SeaBIOS USB boot
must reach original GRUB4DOS 0.4.6a 2024-02-26, report (hd0,0)/FAT16/0x0E or
FAT32/0x0C, list grldr, read its complete CRC32 2b53370c and execute the ECHO marker.
Both private copies missing only grldr must fail this same qualifier with
Cannot find GRLDR in all drives. Snapshot boot must preserve each backing hash.
This models authorization inside a guest; no physical USB or host polkit claim.

Initial failures were test code: a synthetic target admitted only FAT32, a new
readback check needed the existing boot-record oracle import, and a guest edit
referenced a local constant outside its scope. Correct the fixtures, preserve all
other guards and rerun. The first real FAT16 helper creation itself succeeded.
No production formatter/boot/copy defect was observed. Save normal and ASan/UBSan/
leak-detection results, native positive/negative logs, VGA/commands/screenshots,
firmware/input/source hashes and compact manifests before deleting completed media
and builds. Evidence: /home/marius/proj/lufus/.rufus-test-artifacts/cp22n48.

Next CP22n49: expose the now-qualified GRUB4DOS FAT16 route in the existing Qt
filesystem/cluster selection. Preserve original advanced MBR/BIOS policy, retained
ISO restoration, frozen loader before confirmation and one complete helper job.
Verify the native Qt FAT16 route and FAT32 regressions, including final discrete
progress/READY/notification/unplug behavior. NTFS, general GRUB4DOS images, remaining
legacy features, Windows To Go and full Windows parity remain required; unknown
image versions stay unknown. No additional Windows customization qualification.


Final CP22n48 results: all three affected component suites pass in normal and
ASan/UBSan/leak-detection builds: 254 request/process, 82 owned-source and 447 full
helper QtTest passes per build (783 total, including suite setup/cleanup). Both
native FAT16 and FAT32 creation/readback/BIOS/CRC/ECHO and missing-loader controls
pass. Python syntax and scoped whitespace checks pass. Only one existing production
file changes; 694 production hashes remain unchanged and no production file is added.
Compact evidence is verified and all completed CP22n48 temporary builds/media removed.


## CP22n49 — standalone GRUB4DOS FAT16 Qt integration (2026-10-05)

Expose the CP22n48-qualified FAT16 helper route through the existing advanced
Grub4DOS 0.4.6a choice. One production line in NativeFileSystemClusterSizes admits
both FAT filesystems for MBR instead of FAT32 alone; reuse the original shared
filesystem/cluster policy and selection controller. No new GUI/controller/helper,
progress logic or privileged operation; all other 694 production hashes remain
unchanged. On the 64-MiB fixture, FAT16 defaults to 2-KiB clusters, with valid
2/4/8-KiB choices; FAT32 retains 512-byte clusters. NTFS/exFAT/ext and GPT/SFD remain
unavailable for this standalone choice until their backend routes qualify.

Window tests verify original advanced visibility/order, FAT16 default/explicit
FAT32 selection, MBR/BIOS, unavailable-filesystem/layout refusals, the 4-GiB FAT16
capacity limit and remembered FAT16 preference when a smaller device is selected
again. A changed device identity must invalidate selection; test explicit reselection
before expecting its formats. Retained ISO state stays separate from the source-free
request, and hiding the advanced list restores the ordinary image selection.

Extend the same 20 Widgets job scenarios to both filesystems. Require original
cached/downloaded loader bytes/hash before confirmation, exact filesystem/cluster
and confirmation text, no authorization on download/validation/confirmation refusal,
frozen cache replacement, one helper launch, late formatter disappearance, denied
launch, short copy, sync/cleanup/layout/finalize failure and cancellation during
format/boot/finalize. Both FAT variants use Rufus's original five filesystem steps
and discrete 14-slot weighting: Finalizing waits at 60853/65535 without notification;
only joined successful exit produces full READY and one beep/alert. Unplug preserves
completed READY and never repeats notification.

Native Qt qualification creates both FAT16 and FAT32 virtual USBs through real
formatter, original MBR/SBR/standard PBR, private FAT mount, frozen grldr copy and
sync/unmount. Require frozen random Qt serial and label RUFUS_TEST, original records,
exact root-only loader, caller-owned 0700/0400 staging, one guest-only authorization,
held launcher exit, reaped descendants and released claim/mount/staging before READY.
Bare SeaBIOS USB boot reaches original GRUB4DOS 0.4.6a 2024-02-26, reports each exact
FAT16/0x0E or FAT32/0x0C root, reads grldr with CRC32 2b53370c and executes ECHO.
Both missing-loader controls fail the same qualifier with the original diagnostic;
snapshot backing hashes remain unchanged. The helper-only FAT16 regression keeps
its fixed serial/root-label oracle. Host root/polkit and physical USB are not tested.

Temporary work is on the regular filesystem because /tmp is nearly full. Initial
private D-Bus startup failed because that longer path exceeded Unix socket length;
use a short symlink to its disposable tmp directory, keeping the data off /tmp.
Other initial failures were strict FAT32-only simulated-helper checks and the
capacity test's missing explicit reselection. Correct those fixtures, keep their
other guards, rerun and preserve the failed-run evidence. Stop the superseded
failing full suite and its private descendants before completing the corrected run.
Normal full Widgets/window/format regressions and ASan/UBSan/leak-detection focused
GRUB4DOS plus full window/format gates are required. Verify compact logs, native
positive/negative screenshots/commands, source/input/firmware/binary hashes and
manifests, then remove this round's disposable work directory and short tmp symlink.
Evidence: /home/marius/proj/lufus/.rufus-test-artifacts/cp22n49.

Next CP22n50: extend standalone GRUB4DOS to source-free MBR/NTFS using the existing
native formatter, NTFS boot adapter, mount-provider selection and original MBR/SBR/
root loader copy. Audit the current NTFS helper's GRUB2/Windows dispatch, signature,
required frozen loader and completion phases before adding admission. Qualify the
native new-kernel route and original BIOS NTFS loader readback before exposing NTFS
in Qt; retain NTFS-3G fallback requirements and distinguish missing driver support
from real write/device errors. No duplicate formatter/copy/privilege machinery.
General GRUB4DOS images, remaining legacy features, Windows To Go and full Windows
parity remain required. Unknown image versions remain unknown and no additional
Windows customization qualification is scheduled.


Final CP22n49 results: normal full affected suites pass with 56 format-policy,
77 window and 1342 Widgets QtTest passes (1475 total, including setup/cleanup),
with five unchanged conditional Widgets skips. Focused GRUB4DOS scenarios pass
42/42 normally and under ASan/UBSan/leak detection; full sanitized format/window
suites also pass (175 focused/suite passes in total). Native Qt FAT16 and FAT32
creation/readback/BIOS/CRC/ECHO/missing-loader controls and the helper-only FAT16
regression all pass. Python/shell syntax and scoped whitespace checks pass. One
production admission line changes, 694 production hashes remain unchanged, and
no production file is added. Verified compact evidence is saved and completed
work/media/builds plus the temporary short-path alias are removed.


## CP22n50 — standalone GRUB4DOS NTFS helper and native BIOS (2026-10-05)

Source-free MBR/NTFS GRUB4DOS now reuses the existing formatter, NTFS boot adapter,
original GRUB4DOS MBR/8192-byte resource (7680-byte SBR), frozen root loader writer,
private mount/provider selection and single authorized helper. Add the exact
prepare-grub4dos-ntfs-device-v1 operation, keep required private loader/hash/size
metadata and reject ISO/customization/UEFI/SFD/other-loader mixtures. NTFS serial
remains formatter-generated. Extend existing native target admission, formatting
and standard NTFS boot-record guards to GRUB4DOS. Decode must not infer UEFI:NTFS
for this source-free BIOS request. The client must expect checking, partitioning,
formatting, bootloader, mounting, extracting, finalizing, with no premature or
repeated finalizing phase. Loader copying admits the verified private NTFS mount
(kernel or job-owned NTFS-3G FUSE) as well as FAT; FreeDOS remains FAT-only.
There is no new formatter, filesystem writer, privilege machinery or production
module. Seven existing production files change, including two comment-only headers;
688 of the 695 production hashes remain unchanged. Qt remains FAT16/FAT32-only.

Normal affected suites pass: 299 request, 83 owned-loader/source, 461 helper and
56 format-policy QtTest passes, plus 37 loader-copy, 31 FreeDOS-copy and 64 startup
cases (1031 total, including QtTest setup/cleanup). All those suites also pass
under ASan/UBSan with leak detection. Loader-copy tests admit both NTFS providers
and explicitly keep DOS off NTFS. The test-only NTFS-3G regular-file fixture gains
bounded grldr put/get/removal, with child inode ownership matching upstream's
ntfs_delete contract. Sanitized put/get/remove/missing-file and independent clean
metadata readback pass; production does not link this fixture library.

A 64 MiB QEMU removable USB, Linux 7.2.8-arch1-2 and locally built ntfsprogs-plus
mkntfs qualify the real production helper through the preferred kernel ntfs driver.
Require one guest-only authorization, retained original loader, original records and zero remaining SBR gap, NTFS 3.1/4096-byte
clusters, clean volume flags and label RUFUS TEST. Independent 7-Zip and NTFS-3G
fixture readers require exact 322965-byte original grldr, with no other user files.
The helper waits for held launcher exit, syncs/unmounts, removes private staging,
reaps descendants and releases its exclusive claim before reporting completion.
Bare SeaBIOS boot reaches original GRUB4DOS 0.4.6a 2024-02-26, reports NTFS/0x07,
reads the full loader with CRC32 2b53370c and executes ECHO RUFUSGRUB4DOSOK.
Removing only grldr through the fixture library preserves readable NTFS metadata
and makes the same BIOS qualifier fail with the original missing-loader error.
Snapshot disk hashes remain unchanged. Existing FAT32 native/BIOS/control regression
also passes. Host root/polkit, physical USB and OS chainloading remain unqualified.

Initial tests found actual admission/dispatch omissions: implicit UEFI selection,
NTFS client phase inference, native target guards and the FAT-only shared loader
writer. Fix these existing paths rather than add parallel implementations. The
owned-source mutation fixture also relied on two writes having distinct timestamps;
explicitly advance its mtime to make that injected mutation deterministic. A full
sanitized helper run exceeded its 90-second CTest cap during concurrent builds;
rerun its complete executable with the same sanitizers/leak checks, and record
461/461 passes. Preserve failed-run logs as well as final evidence. Export verified
compact manifests, source/binary/tool hashes, logs and BIOS positive/negative
screenshots to /home/marius/proj/lufus/.rufus-test-artifacts/cp22n50, then remove
this round's disposable builds/media and short TMPDIR alias.

Next CP22n51: qualify this standalone GRUB4DOS NTFS job with the job-owned NTFS-3G
fallback when new kernel support is absent. Reuse the existing VM provider recipe,
check one authorization and foreground FUSE lifetime through sync/unmount/exit,
independent original-loader readback and BIOS positive/missing-loader controls.
Keep real mount/write errors as failures rather than switching providers. Only
then expose NTFS in Qt in a separate small checkpoint with original cluster/default
policy and focused Widgets/native completion tests. General GRUB4DOS images, OS
chainloading, Windows To Go and remaining Windows parity remain required. Unknown
image versions remain unknown; no additional Windows customization qualification.


Final CP22n50 evidence is verified: 234 compact files, 33 source and 19 binary
hashes, seven changed production files and 688 unchanged production hashes.
The final native NTFS and FAT32 creation/readback/BIOS/missing-loader recipes pass;
normal and sanitized affected suites each pass 1031 cases. Remove completed work,
raw media, both builds and the short TMPDIR alias, freeing 553.6 MiB allocated.
No checkpoint process remains. Source downloads/dependencies and the user's ISO
are preserved. Continue with CP22n51 fallback qualification before NTFS Qt admission.


## CP22n51 — standalone GRUB4DOS NTFS-3G fallback (2026-10-05)

Qualify the existing production fallback without adding production code. The VM
recipe accepts --grub4dos-ntfs-driver ntfs|lowntfs-3g alongside --grub4dos-ntfs;
its old command form still selects the preferred kernel driver. The fallback guest
uses Linux 7.2.8-arch1-2 without its NTFS module, includes the matched existing
NTFS-3G/library packages and FUSE module, and explicitly requires fsopen("ntfs")
to fail with ENODEV before any job. Require the production helper's NTFS-3G log.
There is no forced provider setting in the job request, new authorization, host
installation or driver-switch logic. All 695 production source hashes are unchanged.

A guest-only wrapper delegates to the real packaged lowntfs-3g with the production
no_detach options and retains its exit briefly after clean native unmount. The
client must remain active with no finished signal and its private loader snapshot
still present. Check the real native process has been reaped, the owned wrapper is
alive and the helper's private mountinfo no longer contains its temporary mount.
Release the wrapper, then independently hold the authorization launcher exit as
before. Require sync/cleanup success, removed private directories/staging, joined
helper/wrapper/native processes and an available exclusive disk claim before the
single completion signal. Each creation has exactly one guest-only authorization.
This is test-only process-lifetime observation; production remains unchanged.

Normal and ASan/UBSan/leak-detection native fallback jobs both pass. Each creates a
64 MiB virtual removable USB with original MBR/SBR/standard NTFS primary records,
backup BPB, clean NTFS 3.1/4096-byte clusters and RUFUS TEST label. Independent
7-Zip and NTFS-3G fixture readers require the exact original 322965-byte grldr and
no other user content. Actual SeaBIOS boot reaches original GRUB4DOS 0.4.6a
2024-02-26, reports NTFS/0x07, reads CRC32 2b53370c and executes ECHO
RUFUSGRUB4DOSOK. Removing only grldr must fail that same boot qualifier with the
original diagnostic; snapshot backing hashes remain unchanged. The preferred
kernel NTFS native/readback/BIOS/control regression also passes using the old CLI
form. Four invalid-provider/option combinations stop before output creation;
Python and embedded Bash syntax plus scoped whitespace checks pass. An initial
parser-check invocation omitted the recipe's required --kernel; fix that invocation
and rerun all four checks. No production defect or native failure was observed.

Evidence is saved at /home/marius/proj/lufus/.rufus-test-artifacts/cp22n51. Preserve
compact logs, source/binary/tool/package/firmware hashes, original-loader manifests
and positive/negative BIOS screenshots before deleting both builds, disposable
media and this checkpoint's TMPDIR alias. The CP22n50 component/failure coverage
remains applicable to unchanged production code; no broad or additional Windows
customization qualification is scheduled. Host root/polkit, physical USB and OS
chainloading are not qualified here. Qt still exposes FAT16/FAT32 for GRUB4DOS.

Next CP22n52: admit NTFS in the existing standalone GRUB4DOS Qt filesystem gate.
Reuse Rufus's existing NTFS default/cluster policy, source-free confirmation and
frozen-loader path; keep formatter-generated serial and BIOS/MBR semantics. Extend
focused format/window/Widgets tests for NTFS selection, cache/signature failures,
No/Cancel, frozen loader, one helper, copy/finalization errors and cancellation,
held exit, original progress, full READY/one notification and stable unplug.
Do not add a second NTFS backend or privilege workflow. Follow with CP22n53 native
Qt NTFS qualification against the already qualified kernel/fallback routes.
General GRUB4DOS images, OS chainloading, Windows To Go and remaining Windows parity
stay required. Unknown image versions stay unknown; no further customization test.


Final CP22n51 result: normal fallback, ASan/UBSan/leak-detection fallback and
preferred-kernel regression all pass native creation/readback/BIOS/missing-loader
controls. All 695 production hashes are unchanged. Verify 139 compact evidence
files, 20 source and 12 binary hashes, then remove completed work/media/both builds
and the short TMPDIR alias, freeing 477.5 MiB allocated. No checkpoint process
remains; downloads, dependency prefixes and the user's ISO are preserved.


## CP22n52 — standalone GRUB4DOS NTFS Qt admission (2026-10-05)

Admit NTFS alongside FAT16/FAT32 in the existing standalone GRUB4DOS cluster
gate. This is the only production change: a named supported-filesystem predicate
in format_options.cpp. All existing Qt selection, source-free request, signed
loader acquisition, native formatter/provider, MBR/SBR, retained loader copy,
progress and authorization operations are reused. Windows code/resources and
CMake source selection remain unchanged; no new backend, abstraction or module.

Use Rufus's existing NTFS 4096-byte default and 4096/8192/16384/32768/65536-byte
choices on the 64 MiB fixture. Preserve an existing NTFS target selection until
an explicit user choice takes precedence. The first supported filesystem still
carries the Default label: FAT16 on this fixture, FAT32 at 4 GiB. A manual FAT16
preference temporarily falls back to FAT32 on a larger device and returns when
FAT16 is supported again. NTFS does not enable UEFI:NTFS for this BIOS-only job;
the formatter supplies its serial and shared label normalization preserves case
and Unicode. A retained ISO is excluded from the frozen source-free request.

The focused Widgets flow covers twenty scenarios for each of FAT16/FAT32/NTFS:
cache/download, No/Cancel, bad/missing signatures or cache, unplug and cancelled
confirmation, disappeared formatter, denied authorization, wrong returned layout,
short copy, missing finalization, sync/cleanup failure and phase cancellation.
Replace cached grldr and change the label after confirmation; the job must retain
its frozen original bytes and normalized label. Require at most one helper start.
Use independent Win32 progress expectations: first finalization is 62414/65535
for NTFS (20/21 original discrete steps), versus 60853/65535 for FAT (13/14).
Hold helper exit after its result; no successful READY or notification is allowed
until joined completion. Then require full green READY, one beep/alert and stable
unplug. Screenshots record NTFS selection, confirmation, finalization and READY.

The original selection test assumed NTFS was unavailable and expected FAT16 for
an existing NTFS target. Update that expectation when admitting NTFS, while
separately checking the default marker and manual preference. An initial follow-up
expectation also incorrectly ignored the explicit FAT16 preference on reselection;
restore the existing FAT32 fallback/FAT16 return expectations. Both were test
expectation errors; no selection-controller or backend fix is needed. An initial
build invocation used an unknown rufus-qt target; use the existing rufus-linux.
Preserve these failed invocations with the final passing evidence.

Normal and ASan/UBSan with leak detection each pass all 56 format cases, 77 window
cases and 62 focused GRUB4DOS flow cases. The full normal Widgets regression result
is recorded below after completion. No additional Windows customization tests.
Evidence is saved at /home/marius/proj/lufus/.rufus-test-artifacts/cp22n52.
CP22n50/51 independently qualify the unchanged native NTFS kernel/FUSE and original
loader BIOS/CRC/missing-loader contracts. This checkpoint uses a private-bus fake
helper for GUI failures and lifetime controls; it does not claim the combined
native Qt NTFS route, host polkit, physical USB or OS chainloading is qualified.

Next CP22n53: qualify native Qt standalone GRUB4DOS NTFS creation using the already
qualified preferred kernel and missing-kernel NTFS-3G fallback. Extend the existing
Qt VM recipe and native NTFS readback oracle rather than adding a second driver.
Require frozen loader/label/formatter serial, original MBR/SBR/PBR, one guest-only
authorization, retained claim and provider/helper/launcher/worker join before full
READY, one notification and stable unplug. Verify independent original-loader
CRC/ECHO BIOS execution and the missing-loader negative control. Keep qualification
bounded to disposable virtual media and clean it after compact evidence export.
General GRUB4DOS images, OS chainloading, Windows To Go and remaining parity stay
required; unknown image versions remain unknown and no new host inference is added.


Final CP22n52 verification: the full normal Widgets regression passes 1362 cases
with five existing skips (read-only real Windows/Alpine ISO inputs unset and three
unknown-Syslinux companion routes). Format/window add 133 passing cases, for 1495
normal cases; the separately focused GRUB4DOS flow passes 62. Sanitized
format/window/focused flow passes 195 cases with ASan/UBSan and leak detection.
GUI build succeeds. Of 695 production files, only format_options.cpp changes;
694 hashes are unchanged and no production file/module is added. Original Windows
sources/resources are unchanged. Native Qt NTFS qualification remains CP22n53.


CP22n52 cleanup: verify compact logs, screenshots, scoped before/after sources and
hash manifests, then remove completed work and both builds plus the short TMPDIR
alias, freeing 643.1 MiB allocated. No checkpoint process remains. Source downloads,
dependency prefixes and the user's ISO are preserved. The evidence export's initial
source list used nonexistent window/protocol paths; correct it to main_window.cpp
and the existing Qt prepare_image_protocol/process sources, then verify all hashes
before cleanup. This was evidence tooling only, with no production/test failure.


## CP22n53 — native standalone GRUB4DOS NTFS Qt route (2026-10-05)

Extend the existing CP22n47/49 Qt driver and CP22n50/51 NTFS readback/BIOS recipe.
Admit --grub4dos-gui with --grub4dos-ntfs; keep old helper and FAT command forms.
No production changes: all 695 hashes are unchanged. Qt selects NTFS from the
shared FAT16/FAT32/NTFS choices and freezes 4096-byte clusters, no UEFI:NTFS,
formatter-owned serial, Rufus ș😀_test normalized label and original cached grldr.
Replace cache bytes before confirmation; native readback must retain the original.
Use one guest-only authorization and the unchanged production helper/formatter.

The preferred guest loads Linux 7.2.8-arch1-2 kernel ntfs. The fallback omits it,
requires fsopen("ntfs") to fail with ENODEV before the job and uses the packaged
real foreground lowntfs-3g. Hold its wrapper after clean native unmount/exit;
require active Qt, no READY/notification, native child reaped, helper-private mount
removed and retained loader staging. Release FUSE, then separately hold launcher
exit at original NTFS 62414/65535 discrete finalization with no notification.
Require successful filesystem/device sync and private cleanup, joined helper/
launcher/provider/worker, removed staging and an available disk claim before full
green READY, one beep/alert and stable unplug. FAT32 keeps 60853/65535.

An extra new test incorrectly required whole-disk exclusivity during the unmounted
FUSE-wrapper hold. Inspection confirms the existing mount handoff closes its disk
claim for mounting and joins FUSE after unmount before reacquiring, revalidating
identity/table and syncing the whole disk. Correct the test to that actual contract;
no production defect or new claim behavior is introduced. Keep the initial failure.
The first sanitized VM exhausted 1 GiB during initramfs unpacking, before Rufus ran.
Use 2 GiB for Qt VM recipes to hold both cpio and its unpacked debug/sanitizer root.
This is test-only VM capacity, without sanitizer suppression or application changes.

Independent native readback checks original MBR/SBR/NTFS primary code and backup
BPB, nonzero native 64-bit serial (Qt request serial remains zero), clean NTFS 3.1,
4096-byte clusters, case-sensitive Unicode label and grldr as the only user file.
7-Zip and the independent test-only NTFS-3G library agree on all 322965 bytes,
SHA256 19ba57329be745ceae46354c5d411b7d8efec6802cfd7f6fee986df4b763f6fe.
Actual SeaBIOS reaches unchanged GRUB4DOS 0.4.6a 2024-02-26, reports NTFS/0x07,
reads CRC32 2b53370c and executes ECHO RUFUSGRUB4DOSOK. Removing only grldr rejects
the same boot check with its original diagnostic. Snapshot backing hashes stay
unchanged. The old FAT32 Qt recipe remains a complete native/readback/BIOS control.

Evidence is saved at /home/marius/proj/lufus/.rufus-test-artifacts/cp22n53. Keep
compact failed/final logs, command/source/binary/provider/tool/firmware hashes,
readback manifests and BIOS positive/negative screenshots before removing work,
media/builds and the short TMPDIR alias. CP22n52's format/window/Widgets failure
coverage applies to unchanged production; no broad rerun or additional Windows
customization test. These virtual-USB jobs model authorization in a guest; they do
not qualify host root/polkit, physical media or OS chainloading/Windows Setup.

Next CP22n54: audit and implement the smallest general GRUB4DOS ISO route using
existing fresh source analysis, original provider precedence, bounded MBR/SBR and
native extraction. Keep an image's grldr distinct from the signed standalone
loader; do not substitute downloaded bytes or infer a version from its filename.
Establish and document the source-loader/boot-record compatibility contract before
admitting a route, retaining gates where it is not established. Reuse the existing
image pipeline without a second formatter/provider/privilege workflow, then qualify
one representative generated image through readback and original loader execution.
Windows To Go, remaining architectures/features and OS chainloading remain required.


Final CP22n53 result: preferred-kernel and absent-kernel NTFS-3G native Qt creation,
independent readback, actual BIOS root/CRC/ECHO and missing-loader controls all
pass normally and with ASan/UBSan/leak detection enabled. The existing FAT32 Qt
command form also passes native/readback/BIOS/control qualification. No sanitizer
diagnostic or production defect was observed. Four invalid option combinations
stop before output creation; Python AST/five embedded Bash syntax checks pass.
All 695 production hashes remain unchanged, with no production file/module added.
The initial exclusivity assertion and early sanitized initramfs-memory failure
remain in evidence. Proceed next with the bounded general-image audit in CP22n54.


CP22n53 cleanup: verify compact native GUI/BIOS/readback evidence and hashes, then
remove completed work/media/both builds and socket alias, freeing 694.3 MiB allocated.
No checkpoint process remains; dependency prefixes/downloads and the user ISO
are preserved. Continue with the bounded general-image audit in CP22n54.


## CP22n54 — general GRUB4DOS BIOS/FAT32 ISO backend (2026-10-05)

Audit the original image contract before enabling the route. Windows selects the
bundled GRUB4DOS MBR/SBR and extracts the image's own root grldr; it does not derive
an image-loader version or replace it with the standalone downloaded loader.
Reuse GetSecondaryBootType precedence (Syslinux, GRUB2, then GRUB4DOS), the original
root-only loader detection, existing bounded SBR writer, native formatting and
retained-source extraction. An unknown image version stays unknown. Matching the
original selection contract is not a claim that every arbitrary loader boots.

Extend the existing GRUB4DOS request operation with its existing retained-source
object. Require exactly one source: frozen standalone grldr or an ISO supplying
its own loader. This first ISO route permits only native BIOS/MBR/FAT32 without
EFI, Windows loaders/customizations or enhanced installation. Standalone FAT16,
FAT32 and NTFS remain unchanged. Decode rejects unsupported ISO filesystem
operations and injected standalone payloads as well as missing source identity.
No new protocol field, formatter, provider, privilege flow or production module.

The helper freshly checks the image and loads the bundled SBR before target access
or unmounts, then uses the existing partition/format/boot/extract/sync/cleanup path.
Only source-free jobs copy the standalone loader. Image jobs use original ISO
progress and require nonzero ISO-copy completion rather than standalone loader
size. Rename the existing Windows-only analysis member to checkedImage because
this check is now shared; Windows behavior and upstream sources/resources stay
unchanged. Six existing production files change; the other 689 hashes match CP53.

Focused ISO/UDF tests cover image-owned root loader, missing/nested loader,
GRUB2/Syslinux precedence, Windows/EFI rejection and changed source before any
target writes. Request/process tests cover roundtrip, filesystem/source/payload
rejection, one launch, successful ISO-copy completion and zero-copy failure.
The first new completion test caught a validator still assuming a standalone
loader; fix that image/standalone distinction. Initial precedence fixtures lacked
a recognizable normal.mod version, so Rufus correctly cleared its GRUB2 flag;
correct the fixture, not original policy. CTest's 60-second ISO and 30-second
request deadlines cut off full runs; retain those logs and complete the same
binaries directly under bounded deadlines, without changing CMake timeouts or
suppressing sanitizers. Final normal and sanitized results are recorded below.

Qt image admission remains gated. CP22n55 must qualify one generated image through
native helper creation, independent target readback and original GRUB4DOS BIOS
execution, including the image's own loader/menu and missing-loader control.
Only then admit and qualify the ordinary Qt ISO route in CP22n56. FAT16/NTFS/EFI
image routes, arbitrary loader compatibility and OS chainloading remain required.
No physical USB, host root/polkit or new Windows customization qualification here.
Compact evidence: /home/marius/proj/lufus/.rufus-test-artifacts/cp22n54. Export and
verify source/binary hashes, scoped diffs and all failed/final logs before cleanup.


Final CP22n54 result: 1,382 ISO/source checks and 302 request/process cases pass
normally and with ASan/UBSan/leak detection enabled, with no sanitizer diagnostic.
Normal Qt/helper and sanitized helper builds pass. Reuse the image analysis already
performed before target access instead of scanning it twice; source identity is
checked again before writes. Native image creation and loader execution are not
yet qualified; keep Qt gated and proceed with CP22n55.


CP22n54 cleanup: verify compact logs, scoped before/after sources, binary/source
hashes and production delta, then remove both completed builds/work and socket
alias, freeing 463.0 MiB allocated. No checkpoint
process remains. Dependencies, source downloads and the user's ISO are preserved.
Proceed with CP22n55 native image/readback/BIOS qualification; Qt remains gated.


## CP22n55 — native GRUB4DOS ISO creation/readback/BIOS (2026-10-05)

Qualify CP22n54's plain BIOS/MBR/FAT32 backend with the existing ISO guest driver,
FAT/MBR readback and GRUB4DOS BIOS oracle. No production changes: all 695 hashes
remain unchanged. Add --grub4dos-image to the existing recipe; no new test driver
or production module. Generate a small ISO with unchanged original root grldr,
menu.lst, image-marker and an 8193-byte payload. The guest has no selected-loader,
selected-core or download cache, so this job must use its own ISO content.

The production client/helper completes checking, partitioning, formatting,
bootloader, mounting, extracting and finalizing with monotonic full ISO copy
progress, one simulated authorization and 331262 copied bytes. Hold launcher exit
after native sync/unmount; the process remains active and its private mount is
already removed. Require joined completion, clean sync/cleanup, unchanged source,
removed staging/private mounts, no public target mount, released disk claim,
restored descriptor count and reaped helper descendants before reporting success.

Independent readback checks original GRUB4DOS MBR, exact 7680-byte SBR after its
512-byte header, zero remaining gap, standard FAT32 records/backup, 1-MiB offset,
66060288-byte filesystem, 512-byte clusters, serial 0x1234abcd and RUFUS_TEST label.
All four target files equal their ISO inputs. The 322965-byte grldr retains SHA256
19ba57329be745ceae46354c5d411b7d8efec6802cfd7f6fee986df4b763f6fe and CRC 2b53370c;
menu.lst CRC is a0dac1ff. Actual SeaBIOS executes original GRUB4DOS 0.4.6a
2024-02-26, reports FAT32/0x0C, reads both CRCs, executes ECHO, reads image-marker
and confirms RUFUSGRUB4DOSIMAGE from the variable set by the ISO's booted menu.
Removing only grldr must reject boot with the original missing-loader diagnostic.
Snapshot backing hashes remain unchanged in both positive and negative boots.

Two initial failures were fixture infrastructure: a variable name exceeded
GRUB4DOS's eight-character limit, then the shared QEMU keyboard helper lacked
punctuation needed by the new commands. Shorten the test variable to RUFUSISO and
add dot/percent/minus to the existing keyboard map. Keep both failed runs and
screens in evidence; no production defect or change was observed. The variable
limit is documented in the upstream README:
https://github.com/chenall/grub4dos/blob/0.4.6a/README_GRUB4DOS_CN.txt

Normal and ASan/UBSan/leak-enabled native creation, readback, original loader/menu
BIOS execution and missing-loader controls all pass. The existing standalone
FAT32 command remains a native/readback/BIOS regression control and passes.
Four invalid recipe combinations stop before output creation; Python AST and
five embedded Bash/guest syntax checks pass. Evidence is saved at
/home/marius/proj/lufus/.rufus-test-artifacts/cp22n55 before deleting disposable
builds/media/socket alias. Dependencies and the user's ISO remain preserved.

Next CP22n56: admit this qualified plain BIOS/FAT32 image route through ordinary
Qt ISO selection, preserving original provider precedence, source retention,
Windows-style filesystem/defaults/progress and durable READY/notification/unplug.
Reuse the existing image controller and generated image recipe; do not acquire
or stage a standalone loader for an ISO. GUI admission remains gated until that
checkpoint. This qualifies one representative original loader/menu, not arbitrary
GRUB4DOS versions or OS chainloading. FAT16/NTFS/EFI image combinations, host
polkit and physical-media durability remain separate required work.


CP22n55 cleanup: verify 214 compact evidence files, 27 source and eight binary
hashes, then remove completed work/media/both builds and socket alias, freeing
482.2 MiB allocated. No checkpoint process remains. Dependency
prefixes/downloads and the user's ISO are preserved. The final recipe refusal
checks include the required build/kernel arguments and assert each specific
diagnostic; initial quick invocations only reached generic missing-argument
errors. Proceed with CP22n56 ordinary Qt ISO admission and completion checks.


## CP22n56 — ordinary Qt GRUB4DOS ISO admission (2026-10-05)

Admit CP22n55's qualified plain BIOS/MBR/FAT32 GRUB4DOS ISO through the existing
Qt selection and image writer. Extend and rename the existing GRUB eligibility
predicate rather than introducing a parallel controller or provider. Rufus's
shared Syslinux/GRUB2/GRUB4DOS precedence, root-loader detection, filesystem policy,
label/cluster defaults and ISO-versus-DD chooser remain authoritative. A plain
eligible image offers MBR, BIOS and FAT32. The small-device default marker still
comes from the original target format policy; do not manufacture a FAT32 default
marker just because it is the only offered filesystem.

Keep FAT16/NTFS and EFI-bearing GRUB4DOS BIOS combinations gated until separately
qualified. This temporary Linux restriction narrows Windows support for a concrete
coverage reason. Preserve the existing GPT/UEFI copy route for EFI-bearing images.
On targets too small for the native FAT32 geometry, suppress unavailable ISO mode
but retain a valid hybrid DD choice. ISO remains recommended when both are valid;
choosing DD uses the existing raw writer and cancelling never starts authorization.

The request retains the ISO's own loader/menu; it neither acquires nor stages a
standalone loader and leaves an unknown image version unknown. Source-backed
GRUB4DOS uses BT_IMAGE progress and the ordinary ISO byte-count completion check.
The original format.c MSG_228 caption covers WriteMBR and WriteSBR, so the ISO
bootloader phase displays "Writing master boot record...". Copying shows the
original ISO percentage rather than the standalone "Copying: grldr" caption.
The shared progress oracle reaches 35061/65535 at half-copy and 64879/65535 at
first finalization for FAT; existing NTFS ISO controls use 37027 and 64224.
Standalone GRUB4DOS captions, staging and discrete progress remain unchanged.

Reuse generated component ISO fixtures and the existing protocol-only helper.
Cover ordinary image loading, GRUB2/Syslinux precedence, EFI fallback, geometry,
filesystem filtering, ISO/DD/cancel, frozen source/no loader cache, one simulated
authorization, copy progress, boot status, result-before-child-exit, sync/cleanup/
copy/layout failures, cancellation, source replacement and unplug. Successful
full READY and one beep/alert require joined completion; unplug after success
remains READY. The fixture never opens the target: these are GUI/component checks,
not a substitute for combined native creation/readback/boot qualification.

The initial focused run had three test-assumption failures: changed device size
correctly invalidated the selected ID, the radio-button center missed its click
indicator, and an earlier GRUB2 row left a cache directory. Reselect the changed
device, click/assert the DD indicator and use a separate GRUB4DOS cache path.
These corrections did not require production changes. Preserve failed and final
logs in /home/marius/proj/lufus/.rufus-test-artifacts/cp22n56.

Only four existing Qt production files change; helper, native backend, protocol,
resources and original Windows sources remain unchanged. Next CP22n57: reuse the
existing Qt/native guest driver, CP22n55 ISO recipe and FAT/BIOS oracles to combine
ordinary Qt image selection with native helper creation, independent readback,
original loader/menu execution and durable READY. No physical USB, host polkit,
arbitrary image-version compatibility, OS chainload or further Windows
customization qualification is claimed here.


CP22n56 final validation: the full ordinary GUI suite passes 1,382 Widgets cases
and 77 window/layout cases, with five existing optional-input skips. This run
preceded the final ISO bootloader caption change and its new targeted case. The
146-case focused selection/creation/failure/GRUB2/standalone regression run passes
with ASan/UBSan/leak detection. On the final caption build the same 146 cases pass
normally and under sanitizers, while the new boot-status case initially expected
success from a fixture deliberately returning ECANCELED. Keep that diagnostic;
correct the test to cancel at the paused phase and require no READY/notification.
The corrected targeted caption/cancellation case passes normally and under
sanitizers. The final sanitized window/layout suite also passes 77 cases. No
additional production change was needed for the fixture expectation. No sanitizer
diagnostic was observed. The four-file production delta leaves 691 of 695
production hashes unchanged. Save compact evidence and remove temporary builds
before continuing with CP22n57's combined native Qt qualification.


CP22n56 cleanup: verify 51 compact evidence files, 33 source hashes and 14 final
binary hashes, then remove both completed builds/work and socket alias, freeing
807.2 MiB allocated. No checkpoint process remains. Dependency
prefixes/downloads and the user's ISO are preserved. Proceed with CP22n57 combined
ordinary Qt/native ISO creation, FAT readback and original GRUB4DOS BIOS execution.


## CP22n57 — native Qt GRUB4DOS ISO creation/readback/BIOS (2026-10-05)

Combine CP22n56's ordinary Qt ISO admission with CP22n55's native helper, generated
ISO, FAT/MBR readback and BIOS oracle. Extend the existing mbr.py recipe with
--grub4dos-image-gui alongside --grub4dos-image and use the existing Widgets guest.
No new production module, helper/provider, test driver or authorization workflow.
The PID-1 guest harness models the same UID-1000 source caller and guest-only
launcher as prior qualification; it does not qualify host polkit or a physical USB.

Load the generated ISO through the ordinary image selector. Require image-owned
root grldr/menu, shared GRUB4DOS provider facts, unknown version retained, MBR/BIOS,
one FAT32 choice, shared source label and 512-byte clusters. Confirm a normal ISO
job without a cached/selected/staged standalone loader, enhanced install or
UEFI:NTFS. Observe the production GUI's monotonic progress, original MBR/SBR
caption and full ISO percentage. Hold launcher exit after native sync/unmount;
require the original ISO finalization position 64879/65535, no full READY and no
completion notification before exit. Then require joined full READY, one beep/
alert, single simulated authorization, clean sync/cleanup, released disk claim,
restored descriptor count, reaped helper descendants, unchanged ISO, removed
private mounts, no public target mount and stable READY/notification on unplug.

Reuse independent readback of original MBR/SBR, zero gap, standard FAT32 records
and backup, geometry, GUI-selected serial/label, and all four files. Reuse actual
SeaBIOS execution of original grldr/menu, CRCs, ECHO, image-marker and the booted
menu variable; removing only grldr must reject boot and snapshot backing hashes
must remain unchanged. Keep the old standalone Qt FAT32/native/readback/BIOS route
as the shared guest driver's regression control. Repeat the ordinary image route
with ASan/UBSan/leak detection. Do not broaden the Windows customization matrix.

Initial build failure: the new test observer used a Qt disconnect overload that
does not accept a function callback. Retain its connection handle for disconnect.
Initial native run reached complete ISO copying and correct finalization, then
failed the test's descriptor comparison because its baseline was taken after
preflight opened the retained ISO. Take the baseline before START to audit the
whole job, including closing that source. Both corrections are test-only; preserve
failed and final evidence at /home/marius/proj/lufus/.rufus-test-artifacts/cp22n57.

Next CP22n58: extend the existing GRUB4DOS image contract/backend to FAT16, reusing
its already-qualified native formatter and original records/SBR plus ordinary
ISO extraction. Keep admission gated until focused source/request checks and
native readback/boot qualification establish the combination. NTFS and EFI BIOS
combinations, arbitrary loader compatibility and OS chainloading remain required.
Unknown image versions remain unknown; a standalone loader identity does not
establish an image version. Physical-media durability and host authorization remain
separate qualifications. No further Windows customization qualification here.


CP22n57 final result: normal and ASan/UBSan/leak-enabled ordinary Qt ISO jobs
pass native creation, joined cleanup/READY, exact disk/files readback, actual
original loader/menu BIOS execution and missing-loader controls. All 331262 ISO
bytes are copied; offset is 1048576 and filesystem size is 66060288. The loader
retains SHA256 19ba57329be745ceae46354c5d411b7d8efec6802cfd7f6fee986df4b763f6fe,
CRC 2b53370c and original 0.4.6a 2024-02-26 identity at boot. Menu CRC a0dac1ff and
RUFUSGRUB4DOSIMAGE from the booted menu variable are verified. Backing snapshots
stay unchanged. Normal standalone Qt FAT32 creation/readback/BIOS and its
missing-loader control also pass. Python AST, five embedded Bash scripts, guest
syntax and four recipe-specific refusals pass. Inspect positive menu execution
and negative missing-loader screens. No production or Windows source changes:
all 695 production hashes remain unchanged, and no sanitizer diagnostic is seen.
Save compact evidence before removing the completed builds/media/socket alias.
Proceed with CP22n58 FAT16 GRUB4DOS image support in the existing backend.


CP22n57 cleanup: verify 170 compact evidence files, 34 source hashes and 12 final
binary hashes, then remove completed builds/guest roots/media and socket alias,
freeing 695.1 MiB allocated. No checkpoint process remains.
Dependency prefixes/downloads and the user's ISO are preserved. Proceed with
CP22n58 FAT16 GRUB4DOS image support and focused backend qualification.


## CP22n58 — GRUB4DOS FAT16 ISO backend contract (2026-10-06)

Extend CP22n54's source-backed GRUB4DOS contract to FAT16 by changing two guards
in the existing Qt/helper protocol implementation. Use the already-defined
prepare-grub4dos-fat16-device-v1 operation with retained ISO metadata and no
standalone loader metadata. Exactly one loader source remains required: retained
image or selected standalone payload. Source-backed NTFS remains refused; EFI,
Windows/other loader precedence, enhanced installation and customization checks
remain unchanged. No new field, operation name, provider, module or native helper
implementation. The original Windows dispatch installs its bundled GRUB4DOS SBR
independently of FAT16/FAT32, and the existing native FAT16 formatter, original
standard records, MBR/SBR writer and ordinary ISO extractor already cover these
operations. GUI admission remains FAT32-only until native FAT16 image qualification.

Extend the existing ISO/UDF preflight rows across FAT16 and FAT32: image-owned
root loader, missing/nested loader, GRUB2/Syslinux precedence, Windows/EFI refusal,
source change, unknown version retention, no standalone metadata and valid
non-started partition geometry. Extend the existing request round trip/process
case to accept FAT16 while retaining rejection of NTFS/unsupported filesystems,
source-plus-loader injection, absent/changed source metadata, caller-supplied
loader bytes and empty copied-byte success. Run focused source/GRUB/legacy/geometry
regressions and the complete existing GRUB request/process suite normally and
with ASan/UBSan/leak detection; build the production GUI and helper both ways.
These checks do not substitute for native FAT16 creation/readback/boot.

The initial concurrent request runs each produced six staging-inventory assertion
failures because both used the same TMPDIR while inspecting rufus-grub2-* staging
directories. No source/request contract case failed. Rerun with a separate
short temporary directory per build. Keep both failed and final logs; isolate
staging-inventory tests in future parallel runs. No production correction was
needed for this orchestration mistake. Evidence:
/home/marius/proj/lufus/.rufus-test-artifacts/cp22n58.

Next CP22n59: reuse the existing native ISO guest and original FAT16/readback/BIOS
oracles to qualify this backend with unchanged image-owned grldr/menu/payload,
one simulated authorization, full copy progress, sync/unmount/launcher joining,
exact files/records and the missing-loader boot control. Keep ordinary Qt FAT16
admission gated until that proof, then extend existing selection and GUI checks.
NTFS/EFI image combinations, arbitrary loader compatibility, OS chainloading,
host polkit and physical-media durability remain required. Do not infer an image
version from the bundled standalone loader. No new Windows customization test.


CP22n58 final validation: 178 focused ISO/source/GRUB/legacy/geometry checks and
all 302 request/helper-process cases pass normally and with ASan/UBSan/leak
detection, with no sanitizer diagnostic. Production GUI and helper builds pass
in both configurations. Only prepare_image_protocol.cpp changes; 694 of 695
production hashes remain unchanged. No native/helper/formatter/shared Windows
code is added or modified, and Qt eligibility remains gated to FAT32. Save
before/after sources, scoped diff, source/binary hashes and failed/final logs
before removing temporary builds. Proceed with CP22n59 native FAT16 ISO proof.


CP22n58 cleanup: verify 35 compact evidence files, 36 source hashes and ten final
binary hashes, then remove both completed builds and all three temporary aliases,
freeing 601.2 MiB allocated. No checkpoint process remains.
Dependency prefixes/downloads and the user's ISO are preserved. Proceed with
CP22n59 native FAT16 image creation/readback/BIOS, keeping Qt FAT16 gated.


## CP22n59 — native GRUB4DOS FAT16 ISO creation/readback/BIOS (2026-10-06)

Qualify CP22n58's FAT16 source-backed contract using the existing ISO guest and
FAT16/GRUB4DOS oracles. No production changes: all 695 hashes remain unchanged.
Allow --grub4dos-image --fat-bits 16 in the existing recipe, preserving the existing
64-MiB target's 2048-byte FAT16 clusters rather than the unrelated 512-MiB GRUB2
fixture's 8192-byte override. Keep the companion Qt image recipe explicitly
FAT32-only until CP22n60 admission. No new driver, module, provider or payload.

Normal and ASan/UBSan/leak-enabled native FAT16 jobs complete seven ISO phases,
monotonic full copy progress, one simulated authorization and joined completion
after native sync/unmount and held launcher exit. Require source unchanged, claim
released, private staging/mounts removed, no public target mount, descriptor count
restored and helper descendants reaped. Readback verifies original GRUB4DOS MBR,
exact bundled SBR prefix and zero gap, original standard FAT16 records, partition
type 0x0E, 1-MiB offset, 66060288-byte volume, 2048-byte clusters, serial 0x1234abcd,
RUFUS_TEST label and all four unchanged ISO files totaling 331262 copied bytes.

Actual SeaBIOS executes original GRUB4DOS 0.4.6a 2024-02-26 on FAT16, reads loader
CRC 2b53370c and menu CRC a0dac1ff, executes ECHO and reads image-marker. The booted
menu variable reports RUFUSGRUB4DOSIMAGE. Removing only grldr fails with the
original missing-loader diagnostic; snapshot backing hashes remain unchanged.
Inspect positive and negative screens. The existing FAT32 native ISO/readback/
BIOS and missing-loader route passes as a regression control. No failed run or
sanitizer diagnostic occurred. Python AST, five embedded Bash scripts, guest syntax
and four recipe-specific refusal checks pass, including the still-gated FAT16
Qt recipe. Evidence: /home/marius/proj/lufus/.rufus-test-artifacts/cp22n59.

Next CP22n60: admit the qualified FAT16 ISO route in the existing Qt selector and
reuse existing component/native Qt tests to verify FAT16/FAT32 choices, original
small-device defaults, ISO/DD, frozen source/no standalone staging, shared ISO
progress, durable READY/one notification and stable unplug. Keep the combined
native Qt/readback/BIOS proof explicit. Source-backed NTFS/EFI combinations,
arbitrary loader compatibility and OS chainloading remain required; unknown
image versions stay unknown. Host polkit/physical-media durability and further
Windows customization installation qualification are not claimed here.


CP22n59 cleanup: verify 168 compact evidence files, 36 source hashes and ten final
binary hashes, then remove completed builds/guest roots/media and socket alias,
freeing 482.7 MiB allocated. No checkpoint process remains.
Dependency prefixes/downloads and the user's ISO are preserved. Proceed with
CP22n60 ordinary Qt FAT16 image admission and combined native GUI qualification.


## CP22n60 — Qt GRUB4DOS FAT16 ISO admission and native qualification (2026-10-06)

Admit CP22n59's qualified FAT16 image route through the existing GRUB selector,
changing only its filesystem candidate guard and qualified mask. Shared Windows
filesystem/cluster policy now supplies FAT16 and FAT32, with the original FAT16
small-device default and Default marker. Keep an explicit FAT32 choice through
reload. An 8-MiB target offers usable FAT16/512-byte clusters; a 2-MiB target is
below the native FAT16 geometry and keeps only a valid hybrid DD route. Preserve
GRUB2/Syslinux precedence, EFI GPT fallback, ISO recommendation/DD/cancel and
source-backed NTFS refusal. No protocol/helper/provider/Windows source change.

Extend existing component creation/failure rows to FAT16, including frozen ISO/
geometry and unknown version, no standalone cache/staging, one authorization,
original 50% copy position 35061/65535 and first-finalizing 64879/65535, MBR/SBR
caption, sync/cleanup/copy/layout/extraction faults, cancellation, source change,
unplug and joined full READY with one beep/alert. Normal and sanitizer focused
Widgets runs pass 159 cases each; window/layout checks pass 77 each. GRUB2 and
standalone GRUB4DOS remain regression controls. No additional Windows
customization installation qualification is performed.

Reuse the existing native Qt guest for FAT16 and the FAT32 regression. Require
the shared FAT16 default and both FAT choices before selecting the requested
filesystem; retain the ordinary ISO confirmation and image-owned loader/menu.
Observe native ISO progress and hold launcher exit after sync/unmount: no READY
or notification until it joins, then full READY/one beep/alert, released claim,
source unchanged, descriptors restored, children reaped, private mounts removed,
no loader cache/staging and stable READY/notification on unplug. Reuse independent
original MBR/SBR/gap/FAT records and all four source files, plus actual BIOS
loader/menu execution and missing-loader rejection. No new driver or module.

The first sanitizer guest did not start: packing its hundreds-of-MiB debug
archive hit the generic 20-second command deadline under concurrent load. Scope
a 60-second named deadline to archive packaging only. The first retry passed it
through the existing wrapper, which already supplies a timeout, and raised a
duplicate-keyword TypeError. Use subprocess.run directly for that one packing
command; preserve both failed logs. These are fixture orchestration issues,
not production failures. Evidence:
/home/marius/proj/lufus/.rufus-test-artifacts/cp22n60.

Next CP22n61: extend the existing GRUB4DOS source-backed contract/backend to NTFS,
reusing its already-qualified native formatter/driver/boot-record operations and
ordinary ISO extraction. Keep Qt admission gated until focused source/request
checks and kernel/fallback native readback/boot prove that combination. EFI image
combinations, arbitrary loader/OS compatibility, host polkit and physical-media
qualification remain required; unknown image versions stay unknown. Preserve the
single Windows customization qualification limit.


CP22n60 final result: ordinary Qt FAT16 creation passes normally and with ASan/
UBSan/leak detection; Qt FAT32 remains a passing native regression. Independent
readback and actual original loader/menu BIOS execution, missing-loader controls
and unchanged snapshot backing media pass for all three jobs. All 331262 source
bytes are copied into a 66060288-byte partition at offset 1048576; FAT16 uses
2048-byte clusters and GUI-owned serial/label. The loader retains SHA256
19ba57329be745ceae46354c5d411b7d8efec6802cfd7f6fee986df4b763f6fe and CRC 2b53370c;
menu CRC a0dac1ff and the booted menu variable are verified. Inspect the sanitizer
positive/negative BIOS screens. No sanitizer diagnostic is seen. Final Python
AST, five embedded Bash scripts, guest syntax and three recipe refusal checks
pass. One existing selector file changes; 694 of 695 production hashes remain
unchanged. Preserve all failed/final fixture logs before cleanup. Proceed with
CP22n61 source-backed NTFS contract/backend work, keeping that Qt route gated.


CP22n60 cleanup: verify 186 compact evidence files, 37 source hashes and 22 final
binary hashes, then remove completed builds/guest roots/media and three temporary
aliases, freeing 1060.4 MiB allocated. No checkpoint process remains.
Dependency prefixes/downloads and the user's ISO are preserved. Proceed with
CP22n61 source-backed NTFS contract/backend work in the existing helper path.


## CP22n61 — GRUB4DOS NTFS ISO contract/backend admission (2026-10-06)

Allow an image-owned loader in the existing prepare-grub4dos-ntfs-device-v1
request. Remove only the encoder's source-backed NTFS exclusion and the decoder's
matching exclusion, including its now-unused predicate. Keep exactly one loader
source: retained ISO metadata or a frozen standalone grldr, never both or neither.
No new protocol operation, module, formatter, installer or privilege workflow.
The existing NTFS helper already loads the original bounded SBR before writes,
checks image policy/source identity before target access, rechecks the source,
formats through the native utility, writes the shared standard NTFS boot record,
extracts the ISO-owned files and joins durable cleanup. Original Windows code
and resources remain unchanged. Only linux/qt/prepare_image_protocol.cpp changes
among 695 production files; Qt still excludes image-backed NTFS.

Extend existing request rows to NTFS alongside FAT16/FAT32: round trip and exact
filesystem/cluster/serial/source retention, shared partition and boot policy,
no replacement core, copied-byte completion, one authorization, empty-copy
rejection, incomplete metadata and unsupported filesystem rejection. Reject
UEFI:NTFS/enhanced/Windows-customization options and caller-supplied NTFS serials.
Extend real generated ISO/UDF source rows to NTFS: own/missing/nested loader,
GRUB2 and Syslinux precedence, Windows and EFI refusal, and stale-source rejection
before target creation. Unknown image versions remain unknown; never replace the
image loader with the standalone payload.

Build the existing helper and both suites normally and with ASan/UBSan and leak
detection, using separate temporary roots. Each run passes all 1414 ISO/source
checks and all 302 request checks, with no skips or sanitizer diagnostics. These
are component/source contract results, not native NTFS creation/boot qualification.
No physical target or additional Windows customization installation is tested.
Compact evidence: /home/marius/proj/lufus/.rufus-test-artifacts/cp22n61.

Next CP22n62: extend the existing native GRUB4DOS ISO recipe/readback/BIOS oracle
to NTFS with the preferred kernel ntfs driver and explicit lowntfs-3g fallback.
Reuse the generated image-owned loader/menu and all source files; check original
MBR/SBR/gap/standard NTFS records, content/geometry, actual menu execution,
missing-loader rejection, source/backing-media preservation, one authorization
and durable joined cleanup. Include sanitizer qualification. Keep Qt NTFS gated
until that combination passes; ordinary GUI admission follows separately. EFI,
arbitrary image/OS compatibility, host polkit and physical-media qualification
remain required. Preserve the single Windows customization qualification limit.


CP22n61 cleanup: verify compact logs/scoped changes, 21 source hashes and eight
final binary hashes, then remove the completed normal/sanitizer builds and two
temporary aliases, freeing 399.1 MiB allocated. No checkpoint process remains.
Dependency prefixes/downloads and the user ISO are preserved. Proceed with
CP22n62 native NTFS image qualification; Qt admission stays gated.


## CP22n62 — native GRUB4DOS NTFS ISO creation/readback/BIOS (2026-10-06)

Reuse the existing GRUB4DOS ISO recipe for NTFS by selecting its native NTFS
helper/provider flags and readback branch. Extend its independent NTFS checks
from the standalone grldr to all four source files, checked by both 7z and the
retained NTFS-3G reader. Admit menu.lst, image-marker and payload.bin only for
readback in that test-only reader; new names cannot be populated or removed,
traversal remains refused, and the existing file ownership/size guards remain.
No production code changes: all 695 production hashes remain unchanged.

Reuse the existing held FUSE-exit and joined-process audits in named operations
for both standalone and ISO jobs. The fallback guest proves kernel ntfs support
is absent, then uses real foreground lowntfs-3g inside the same authorized helper.
After native unmount/child exit, its held wrapper must keep the image client
active, with no private target mount or standalone loader snapshot. Release it,
then separately hold the authorization launcher; successful completion requires
both joins, released claim, restored descriptors, reaped descendants, unchanged
source and removed private mounts/staging. Keep one simulated authorization and
seven native phases, with monotonic byte progress ending at its exact total.

Normal and ASan/UBSan/leak-detection NTFS ISO jobs pass with kernel ntfs and
lowntfs-3g. FAT32 ISO and standalone NTFS fallback are passing native regression
controls. Independent readback checks original MBR/SBR/gap/standard NTFS records,
0x07 partition, offset 1048576, size/filesystem bytes 66060288, 4096-byte clusters,
formatter-owned serial, label RUFUS_TEST, clean NTFS flags and exact four-file
content. Copy all 331262 source bytes, including original 322965-byte grldr.
Actual SeaBIOS execution reads the loader CRC 2b53370c, menu CRC a0dac1ff and
image marker; the menu variable proves execution rather than merely copying it.
Deleting only grldr produces the original missing-loader diagnostic. Positive
and negative snapshot boots leave their backing media unchanged. Inspect the
sanitizer NTFS menu and missing-loader screenshots; no sanitizer diagnostic.

Initial fixture failures are preserved: the FAT-qualified 6.18.55 kernel has no
new ntfs module, so the kernel guest was not started. Use retained Arch 7.2.8 for
kernel ntfs; keep 6.18.55 for fallback absence qualification. Both initial native
NTFS creations then succeeded, but the independent reader refused image-marker
with status 64 because its old filename list covered only standalone/Windows
files. Extend that read-only list and rerun both NTFS jobs. No production fix was
needed. Final Python AST, five embedded Bash scripts and guest syntax pass;
five invalid recipe combinations fail before output creation and 12 normal/
sanitizer reader filename/operation guards pass. Compact evidence:
/home/marius/proj/lufus/.rufus-test-artifacts/cp22n62.

Next CP22n63: admit NTFS for ordinary Qt GRUB4DOS ISO selection through the
existing candidate guard and qualified filesystem mask. Keep shared FAT16
small-device defaults and retained manual choices. Extend existing component
creation/failure/progress/READY checks, then qualify the ordinary native Qt ISO
route with kernel ntfs and explicit lowntfs-3g, including held provider/launcher
exit and joined full READY/one beep/alert. Reuse current native readback/boot
oracles and FAT regression controls. Qt NTFS stays gated until this checkpoint.
Unknown image versions stay unknown; no loader substitution or new host-setting
inference. Arbitrary image/OS compatibility, EFI combinations, physical hardware
and real host polkit remain required. Keep the one-customization test limit.


CP22n62 cleanup: verify 324 compact evidence files, 29 source hashes and 12 final
binary hashes; remove completed builds/guest outputs and three temporary aliases,
freeing 483.6 MiB allocated. Native recipes already removed their disposable
media. No checkpoint process remains. Dependencies/downloads and the user ISO
are preserved. Proceed with CP22n63 ordinary Qt NTFS ISO admission/qualification.


## CP22n63 — ordinary Qt GRUB4DOS NTFS ISO admission/qualification (2026-10-06)

Admit NTFS through the existing GRUB image candidate loop and shared FAT16/FAT32/
NTFS mask. Remove the two GRUB4DOS-specific exceptions and redundant provider
mask selection; do not add a backend, module, callback or compatibility layer.
Shared small-device FAT16 defaults/Default labels remain unchanged, native
geometry excludes unusable filesystems and manual NTFS/FAT32 choices survive
image reload. Keep original Syslinux/GRUB2 precedence, plain BIOS eligibility,
ISO recommendation/DD/cancel and image-owned loader/version behavior. Original
Windows code, boot resources, request protocol and native helper remain unchanged.

A new GRUB4DOS NTFS short-format GUI case exposes a real completion-validator
omission: before the fix, a successful helper result reporting 512 bytes less
than the confirmed NTFS extent reaches Complete instead of Failed. Preserve that
failed test. Extend the existing exact native-format predicate to GRUB4DOS,
covering standalone and source-backed jobs. Both short-format cases now remain
FAILED with the complete-partition diagnostic, no full READY/beep/alert. This is
a GUI result-validation fix, not a native formatting failure. Two production
files change, with 693 of 695 hashes unchanged; overall production code shrinks.

Extend the existing helper fixture and creation/failure rows to NTFS, including
4-KiB/8-KiB clusters, formatter-owned serial, source metadata/no cache/core,
original 50% ISO progress 37027/65535 and first-finalizing 64224/65535, original
MBR/SBR caption, complete copy/layout/format, authorization denial, sync/cleanup/
extraction failures, cancellation, source change and unplug. Normal and ASan/
UBSan/leak-detection focused GUI runs pass 173 cases each; window/layout suites
pass 77 each. Standalone GRUB4DOS and GRUB2 remain regression controls. A new
assertion initially called a nonexistent problem() getter and failed both test
builds; use the existing public statusText(), without adding a production API.

Reuse the ordinary native Qt image guest for NTFS and both providers, admitting
its existing companion recipe flag. Require three filesystem choices and shared
FAT16 default before selecting NTFS. Freeze the NTFS geometry/zero caller serial,
plain BIOS/source-owned/no cached loader request, and confirmation before one
simulated authorization. Observe monotonic native ISO byte/GUI progress. The
fallback omits kernel ntfs, uses real foreground lowntfs-3g and holds its wrapper
after native unmount/exit: no READY, notification, private mount or standalone
snapshot. Then hold the helper launcher separately. Only after all joins show
full READY with one beep/alert; require released claim, restored descriptors,
reaped descendants, removed private mounts/staging, unchanged source and stable
completed READY on unplug. Keep no standalone loader cache/version inference.

Normal and sanitizer Qt NTFS runs pass with retained Arch 7.2.8 kernel ntfs and
6.18.55 lowntfs-3g fallback. Ordinary Qt FAT32 ISO and standalone Qt NTFS fallback
also pass as native regressions. Reuse independent original MBR/SBR/gap/standard
NTFS records, exact four-file checks through both 7z/NTFS-3G, clean filesystem,
formatter-owned serial, partition offset 1048576/size 66060288 and 4096-byte
clusters. Copy all 331262 source bytes. Actual BIOS loader/menu CRC, menu-variable
and marker execution and missing-loader rejection pass; snapshot backing media
stay unchanged. Inspect final sanitizer NTFS menu and fallback missing-loader
screens. One initial sanitizer kernel negative control timed out while deleting
grldr in the test volume under concurrent VM load; GUI creation, readback and
positive BIOS boot had passed. A full isolated rerun passes with unchanged code
and the same 15-second control deadline. Preserve failed and final logs. No
sanitizer diagnostic. Python AST, five embedded Bash scripts, guest syntax and
five invalid recipe refusals pass. Evidence:
/home/marius/proj/lufus/.rufus-test-artifacts/cp22n63.

Next CP22n64: inspect the shared GRUB4DOS combined BIOS/UEFI image policy and
extend the source/backend contract first for FAT32 with an image-owned EFI
fallback. Reuse original MBR/SBR, existing FAT records and ordinary extraction;
require source/architecture/layout checks and keep GUI admission gated until
native BIOS/UEFI proof. Handle required NTFS UEFI:NTFS pairing separately after
its contract is established. No new host-setting inference or loader-version
substitution. General image/OS compatibility, host polkit, physical hardware
and delivery work remain required. Keep the one-customization test limit.


CP22n63 cleanup: verify 355 compact evidence files, 36 source hashes and 24 final
binary hashes; remove completed normal/sanitizer builds, guest outputs and three
temporary aliases, freeing 1061.1 MiB allocated. Native recipes already removed
their disposable media. No checkpoint process remains. Dependencies/downloads
and the user ISO are preserved. Proceed with CP22n64 combined BIOS/UEFI FAT32
source-contract work; no physical USB or extra Windows customization test.


## CP22n64 — GRUB4DOS combined FAT32 BIOS/UEFI source contract (2026-10-06)

Rufus's shared filesystem/layout policy permits a non-Windows GRUB4DOS image on
FAT32 with ordinary EFI files and no extra partition. Narrow the Linux source
check's blanket EFI refusal to the unqualified FAT16/NTFS combinations, then
include the FAT32 combined route in the existing fallback validation step.
Reuse checkFallbackEfi and its bounded original-case ISO/UDF reader, x64 PE32+
EFI-application checks and path/architecture validation. No new EFI validator,
protocol operation/field, installer, extractor or backend. One existing source
validator changes; 694 of 695 production hashes remain unchanged. Original
Windows code/resources, native helper and Qt selector are unchanged. Existing
GPT/UEFI behavior remains separate; combined MBR Qt admission stays gated.

Require an image-owned root grldr, shared Syslinux/GRUB2 precedence, complete
analysis and unchanged source identity before target access. Unknown loader
versions stay unknown; do not replace the source loader. FAT32 combined requests
retain standard FAT boot records, original GRUB4DOS MBR/SBR selection, no core/
loader staging, no UEFI:NTFS payload, no WIM split/Windows repair and no extra
partitions. FAT16 and NTFS combined paths remain explicit qualification work;
the temporary FAT32 gate is not a claim that Windows disallows them. The existing
native EFI reader currently qualifies x64 fallback applications; other target
architectures remain required for parity, with no new host inference.

Extend existing ISO/UDF prerequisite rows to accepted FAT32 EFI and mixed-case
fallbacks, while retaining FAT16/NTFS refusal. Assert exact retained fallback
path, shared layout/zero extra payload, filesystem extraction limit, no version
inference and no target creation. Reuse the existing 14 malformed EFI/path data
rows for GRUB4DOS across both ISO and UDF, without copying their header-generation
logic: vendor/other-architecture paths, empty/truncated/bad DOS/PE/machine,
header overflows, Windows/EFI-driver subsystem, PE32 and invalid optional headers
must all fail before extraction/target access. Check fallback read errors,
cancellation and source changes in both views. Reuse one named test request
setup operation, without adding a production API. Existing per-check descriptor
and target-preservation audits remain in force.

Build the existing helper and full source/request suites normally and with
ASan/UBSan/leak detection in separate temporary roots. Each run passes all 1440
source checks and 302 request checks, with no failures/skips or sanitizer
diagnostics. These are source/backend contract results, not native creation or
firmware boot qualification. No physical target or additional Windows
customization installation test. Evidence:
/home/marius/proj/lufus/.rufus-test-artifacts/cp22n64.

Next CP22n65: extend the existing generated GRUB4DOS ISO native fixture with an
image-owned x64 EFI application. Reuse FAT32 helper/source/progress/cleanup,
original boot-record/four-file readback and GRUB4DOS BIOS menu oracle; also read
back the extra EFI file and execute it through existing UEFI firmware/handoff
checks. Require unchanged snapshot backing media, one authorization and durable
joined cleanup. Include sanitizers and a plain BIOS regression. Keep combined
MBR Qt admission gated until that source/native BIOS/UEFI route passes. FAT16,
NTFS required UEFI:NTFS pairing, arbitrary loader/OS compatibility, other EFI
architectures, real host polkit and physical-media/delivery work remain required.
Preserve the one-customization test limit.


CP22n64 cleanup: verify 30 compact evidence files, 25 source hashes and eight
final binary hashes; remove completed normal/sanitizer builds and two temporary
aliases, freeing 399.2 MiB allocated. No checkpoint process remains. Dependencies/
downloads and the user ISO are preserved. Proceed with CP22n65 native combined
FAT32 BIOS/UEFI creation/readback/boot; combined MBR Qt admission remains gated.


## CP22n65 — native GRUB4DOS FAT32 BIOS/UEFI creation/readback/boot (2026-10-07)

Extend the existing generated GRUB4DOS ISO fixture with the already-built x64
uefi_handoff_fixture at EFI/BOOT/BOOTX64.EFI. Add a test-only companion
--grub4dos-image-uefi flag: require the image route, FAT32, built EFI application
and no combined MBR Qt option yet. Retain image-owned original grldr/menu and
unknown source version. Reuse the actual production helper/source preflight,
original MBR/SBR/standard FAT records, ordinary extraction and durable cleanup.
No production file changes; all 695 production hashes remain unchanged.

Extend existing source manifests/readback to recursive leaf paths, without a
new driver or reader. Check all five files byte-for-byte, including the nested
EFI application, exact copy total 333310 bytes, original MBR/SBR/gap/standard FAT
records, 0x0C partition, offset 1048576/size 66060288, 512-byte clusters, fixed
serial/label and exact geometry. The EFI application is 2048 bytes; its hash is
captured per build. Keep seven native phases, monotonic ISO progress, one
simulated authorization, held launcher after sync/unmount, joined result,
released claim, restored descriptors, reaped descendants and unchanged source.
The image has no selected standalone loader/core or extra UEFI:NTFS partition.

Normal and ASan/UBSan/leak-detection combined jobs pass native creation/readback,
actual SeaBIOS original loader/menu CRC/variable/marker execution and original
missing-grldr rejection. Reuse boot_syslinux_uefi(native_fat=True) and its existing
OVMF/application oracle on the same verified FAT32 disk: the source-owned x64
fallback executes, emits the shared RUFUS_SYSLINUX_UEFI_OK fixture marker and
exits through the emulator with status 33. Require no UEFI:NTFS banner and
unchanged snapshot backing media. The marker names the shared test application,
not a Syslinux installation. Plain BIOS FAT32 remains a passing native regression
with its original four files/331262-byte total. Inspect the sanitizer BIOS menu
screen and UEFI serial firmware/application evidence. No sanitizer diagnostics.
Firmware/application execution is representative proof, not OS chainload,
Secure Boot, other architecture or physical-device qualification.

An initial test-only duplicate source-preflight call caused undefined-reference
link failures because the guest did not link that validator. Remove the duplicate
call instead of adding a new test dependency: the actual helper already checks
the contract, and independent content/firmware checks prove its result. Retain
both failed build logs. Final Python AST, five embedded Bash scripts, guest
syntax and five invalid recipe refusals before output creation pass. Native
cases all pass without production or deadline changes. Evidence:
/home/marius/proj/lufus/.rufus-test-artifacts/cp22n65.

Next CP22n66: admit the qualified FAT32 combined MBR route in ordinary Qt image
selection, using shared partition/target/filesystem policy and existing image
writer. Preserve plain BIOS FAT16/FAT32/NTFS choices and GPT/UEFI behavior; keep
unqualified combined FAT16 and NTFS UEFI:NTFS routes gated. Extend existing GUI
selection/ISO-DD/creation/failure/progress/durable READY checks, then qualify the
ordinary native Qt combined FAT32 route normally and with sanitizers using these
same BIOS/UEFI/readback oracles. No host inference, source-loader replacement or
extra customization installation test. Other architectures, FAT16, NTFS paired
EFI, arbitrary loader/OS compatibility, real host polkit, physical media and
delivery remain required for parity.


CP22n65 cleanup: verify 179 compact evidence files, 27 source hashes and 12 final
binary hashes, retaining both EFI fixture binaries/firmware manifests; remove
completed builds/guest outputs and three temporary aliases, freeing 482.1 MiB
allocated. Native recipes already removed disposable disks/guest roots. No
checkpoint process remains. Dependencies/downloads and the user ISO are preserved.
Proceed with CP22n66 ordinary combined MBR Qt FAT32 admission/qualification.


## CP22n66 — ordinary Qt GRUB4DOS FAT32 BIOS/UEFI route (2026-10-07)

Admit combined MBR FAT32 through the existing GRUB candidate loop and qualified
filesystem mask. Keep that mask scoped to MBR, so existing GPT/UEFI choices remain
available. Shared policy preserves the pure EFI image's GPT default and explicit
MBR choice, and hybrid ISO images' MBR default. Selecting MBR offers qualified
FAT32 and BIOS or UEFI; plain BIOS still offers FAT16/FAT32/NTFS. Preserve Win32's
Default marker calculation before image filtering: on this 64-MiB target the
physical default remains FAT16, so the sole combined FAT32 choice has no Default
suffix. Manual scheme/format choices survive reload. Unqualified combined FAT16
and NTFS remain required work, not permanent differences from Windows.

Only linux/qt/selection_controller.cpp changes among 695 production files; 694
hashes stay unchanged. Existing target/confirmation rendering already uses image
EFI facts, and the writer/helper/source validator already implement this route.
No new UI/backend/module/protocol/provider or Windows change. The source owns its
grldr/menu/EFI fallback and loader version stays unknown; no cache substitution
or new host-setting inference.

Extend existing component tests for GPT/MBR switching and labels, combined
FAT32-only choices, retained manual selection, unchanged GPT NTFS availability,
ISO recommendation/DD/cancel for combined hybrids, frozen source/FAT32/EFI path,
original copy/progress/finalization and joined READY. Authorization denial,
sync/cleanup/extraction/copy/layout failures, cancellation, stale source and
unplug remain failures. Direct unqualified combined FAT16/NTFS requests fail
before confirmation/authorization. Normal and ASan/UBSan/leak-detection focused
GUI runs each pass 189 cases; window/layout runs pass 77 each. No failed builds,
fixture attempts or sanitizer diagnostics in this checkpoint.

Reuse the native ordinary Qt ISO guest: load the generated combined source,
retain GPT as default, explicitly choose MBR, require BIOS or UEFI/FAT32 and
frozen source-owned x64 fallback with no extra payload/partition/core/cache.
Observe native ISO progress, original 64879/65535 first-finalizing state, held
launcher after sync/private unmount and no READY/notification until it joins.
Only then full READY/one beep/alert, released claim, restored descriptors, reaped
children, removed private mounts/staging, unchanged source and stable completed
unplug. Root PID1/UID1000 modeling and one guest-only authorization launcher
remain the existing harness; this does not qualify real host polkit.

Normal/sanitizer combined Qt FAT32 jobs pass native five-file/original record
readback, original GRUB4DOS BIOS menu/CRC/variable execution, missing-loader
rejection and direct FAT32 OVMF/source EFI application handoff with no UEFI:NTFS.
Copy all 333310 source bytes into offset 1048576/size 66060288 with 512-byte
clusters and GUI-owned serial/label. Both firmware snapshot backing disks stay
unchanged. Plain BIOS Qt FAT32 remains a passing native regression at 331262
bytes. Inspect the sanitizer BIOS menu screen and shared UEFI application marker/
status33 evidence. Final Python AST, five embedded Bash scripts, guest syntax and
five invalid recipe refusals before output creation pass. Evidence:
/home/marius/proj/lufus/.rufus-test-artifacts/cp22n66.

Next CP22n67: extend the combined source/backend contract to FAT16 using the same
bounded EFI validator, shared layout and original FAT16/GRUB4DOS records. Reuse
the generated native image/readback/BIOS/UEFI oracles for FAT16, with sanitizers
and FAT32 regression, before ordinary combined FAT16 GUI admission. Required
NTFS UEFI:NTFS pairing follows separately. Other architectures, arbitrary loader/
OS compatibility, Secure Boot, real host polkit, physical media and delivery
remain required. Preserve the one-customization qualification limit.


CP22n66 cleanup: verify 187 compact evidence files, 32 source hashes and 24 final
binary hashes, retaining EFI binaries/firmware manifests; remove completed
normal/sanitizer builds, guest outputs and three temporary aliases, freeing
1059.8 MiB allocated. Native recipes already removed disposable disks/guest roots.
No checkpoint process remains. Dependencies/downloads and the user ISO are
preserved. Proceed with CP22n67 combined FAT16 source/native qualification.


## CP22n67 — combined GRUB4DOS FAT16 source/native BIOS/UEFI proof (2026-10-07)

Extend the existing combined-image source predicate from FAT32 to FAT16/FAT32
and route both through the existing bounded x64 EFI fallback validator. Reuse
shared filesystem/layout policy, original FAT16/GRUB4DOS MBR/SBR/standard PBR,
ordinary ISO extraction and complete-job cleanup. No new validator, installer,
formatter, protocol field/operation, provider or module. Only the existing
linux/platform/iso_write_check.cpp changes among 695 production files; 694 hashes
remain unchanged. The Qt selector still exposes FAT32 alone in combined MBR mode.
NTFS paired EFI, other architectures and broader compatibility remain required.

Extend current ISO/UDF positive/mixed-case rows to FAT16 and reuse all 14 invalid
EFI/path cases across both FAT filesystems/views. Extend read-error/cancellation/
stale-source rows to FAT16. Source checks preserve descriptor counts, no target
access, image-owned loader/fallback, unknown version, standard layout and no
UEFI:NTFS/core/cache/Windows repair. Full normal/sanitizer source suites pass 1446
cases each; request suites pass 302 each. Existing GUI selection/failure checks
pass 94 each, including the unchanged FAT32-only combined choices and GPT NTFS
behavior. Remove one obsolete direct-backend FAT16 refusal row: that backend
contract is now valid; the actual GUI choice remains hidden, and direct NTFS
combined refusal remains checked. This is test maintenance for an intentional
backend admission, not removal of GUI filtering. No failed attempts or sanitizer
diagnostics. Preserve the one-customization qualification limit.

Reuse --grub4dos-image --grub4dos-image-uefi with --fat-bits 16 in the existing
native driver. Keep combined FAT16 GUI companion refused before output creation
until its own admission/qualification. Require FAT16/FAT32, not NTFS, in the
native request audit; reuse the same five-file manifest/recursive readback and
BIOS/UEFI oracles. No production helper changes. Normal and ASan/UBSan/leak-
detection FAT16 native jobs pass exact five-file/original record readback, copy
333310 bytes at offset 1048576/size 66060288, 2048-byte clusters and 0x0E table
entry, serial/label, seven phases, monotonic copy progress, one simulated
authorization, held launcher, sync/private unmount and joined completion with
released claim/descriptors/descendants/source identity.

Actual SeaBIOS original 0.4.6a loader/menu CRC/variable/marker execution and
missing-grldr rejection pass. The same verified FAT16 disk boots its source-owned
x64 EFI application directly under OVMF, emits the shared application marker and
exits with emulator status 33, with no UEFI:NTFS banner. Both firmware snapshot
backing disks stay unchanged. Combined FAT32 is a passing native regression.
Inspect sanitizer FAT16 BIOS menu screen and UEFI application serial/status
proof. This is representative loader/firmware proof, not an OS chainload, Secure
Boot, other architecture, host polkit or physical-device qualification. Python
AST, five embedded Bash scripts, guest syntax and five invalid recipe refusals
before output creation pass. Evidence:
/home/marius/proj/lufus/.rufus-test-artifacts/cp22n67.

Next CP22n68: admit qualified combined FAT16 in ordinary Qt MBR selection using
the existing candidate/mask, preserving original defaults and manual FAT16/
FAT32 choices. Extend existing component/native GUI progress/failure/joined READY
checks, then run ordinary FAT16 creation/readback/BIOS/UEFI normally and with
sanitizers with FAT32 regression. Preserve GPT/UEFI and plain BIOS behavior.
Required NTFS UEFI:NTFS pairing follows separately; other architecture/OS/Secure
Boot, real host authorization, hardware and delivery work remain required. Do
not infer loader versions, substitute source payloads or add host preferences.


CP22n67 cleanup: verify 195 compact evidence files, 29 source hashes and 22 final
binary hashes, retaining EFI binaries/firmware manifests; remove completed
builds/guest outputs and three temporary aliases, freeing 828.4 MiB allocated.
Native recipes already removed disposable disks/guest roots. No checkpoint
process remains. Dependencies/downloads and the user ISO are preserved. Proceed
with CP22n68 ordinary combined FAT16 Qt admission/qualification.


## CP22n68 — ordinary Qt combined GRUB4DOS FAT16/FAT32 (2026-10-07)

Admit the CP22n67-qualified combined FAT16 route by changing only the existing
GRUB candidate's NTFS exclusion and combined MBR FAT mask. One production file
changes, with 694 of 695 hashes unchanged. No new UI/backend/validator/provider/
protocol/helper or Windows change. Existing GPT/UEFI and plain BIOS behavior
remain; combined NTFS UEFI:NTFS pairing is still required work. Source-owned
loaders/fallbacks remain intact and versions unknown; no host-setting inference.

With FAT16 eligible, shared Win32 preselection/fallback naturally chooses FAT16
(Default) on the 64-MiB combined MBR target, using 2048-byte clusters. Preserve
the GPT default for non-hybrid EFI images and explicit MBR choice, shared BIOS
or UEFI target/confirmation, and manual FAT16/FAT32 choices through reload. GPT
NTFS remains available; combined MBR NTFS stays filtered and direct requests
refused. The initial normal/sanitizer focused GUI runs each show 24 failures:
the old tests still expect the temporary FAT32-only default. Correct test/
native-observer expectations to the unchanged shared FAT16 behavior; do not add
a production default override. Preserve both failed logs. Final focused GUI
runs pass 199 each; window/layout runs pass 77 each, with no skips/sanitizer
diagnostics. ISO recommendation/DD/cancel, frozen geometry/source/fallback,
progress, copy/layout/extraction/sync/cleanup failures, cancellation, stale source,
unplug and incomplete NTFS formatting remain covered, alongside GRUB2 and
standalone GRUB4DOS regressions.

Reuse the ordinary native Qt guest and existing five-file/FAT16/original-record/
BIOS/OVMF oracles. Require shared GPT default, both FAT choices and the Default
marker after choosing MBR, then select/freeze the requested FAT16/FAT32 geometry
and source-owned x64 fallback with no extra payload/partition/core/cache. Native
ISO progress reaches 100%; held launcher after sync/private unmount remains at
original FAT first-finalizing 64879/65535, without READY/notification. Only joined
completion gives full READY and one beep/alert. Audit released claim, restored
descriptors, reaped descendants, removed staging/mounts, source unchanged and
stable completed unplug. One guest authorization models UID1000/root PID1; real
host polkit and physical hardware remain unqualified.

Normal/sanitizer combined FAT16 Qt creation/readback/BIOS/UEFI passes, with FAT32
native Qt regression. Exact copy total 333310 bytes at offset 1048576/size 66060288;
FAT16 uses 2048-byte clusters/0x0E table, GUI-owned serial/label and original MBR/
SBR/standard FAT records. BIOS loader/menu CRC/variable/marker execution and
missing-loader rejection pass. Direct FAT source EFI application executes with
shared marker/emulator status33 and no UEFI:NTFS; all snapshot media stay
unchanged. Inspect the final sanitizer FAT16 menu screen and EFI serial proof.

Initial normal FAT16/FAT32 emulator startups time out waiting for QMP after
successful GUI creation/readback under concurrent GUI/VM load. Isolated normal
FAT16 passes unchanged. The first isolated FAT32 retry gets further but its
captured input echo is missing letters in echo %RUFUSISO%; this is injected-key
loss, not a copied-menu failure. Give only the GRUB4DOS oracle 40-ms key holds/
80-ms spacing through optional parameters on the existing shared key helper;
other callers keep their original defaults. Re-run all three native jobs
individually with final key timing. Firmware/command deadlines remain unchanged;
retain failed/final logs/screens. Final Python syntax, five embedded Bash scripts,
guest syntax and five invalid recipe refusals before output creation pass.
Evidence: /home/marius/proj/lufus/.rufus-test-artifacts/cp22n68.

Next CP22n69: establish source-backed GRUB4DOS NTFS BIOS/UEFI request/backend
pairing with the existing bundled UEFI:NTFS payload. Reuse the GRUB2 paired
operation/validation pattern, original GRUB4DOS/NTFS records, shared extra-
partition policy and source-owned EFI/loader validation. Keep combined NTFS GUI
admission gated until request/source and native kernel/fallback/readback/firmware
proof. Other architectures, arbitrary OS/loader compatibility, Secure Boot,
real host authorization, physical media and delivery remain required. Preserve
the one-customization qualification limit.


CP22n68 cleanup: verify 358 compact evidence files, 33 source hashes and 24 final
binary hashes, retaining failed/final logs and EFI binaries/firmware manifests;
remove completed builds/guest outputs and three temporary aliases, freeing
1061.0 MiB allocated. Native recipes already removed disposable disks/guest roots.
No checkpoint process remains. Dependencies/downloads and the user ISO are
preserved. Proceed with CP22n69 GRUB4DOS NTFS UEFI:NTFS request/backend pairing.


## CP22n69 — GRUB4DOS NTFS UEFI:NTFS request/source pairing (2026-10-07)

Reuse GRUB2's paired NTFS image-request pattern for source-owned GRUB4DOS. Add
prepare-grub4dos-ntfs-uefi-device-v1 as the distinct operation discriminator,
without a new field, module, installer or provider. Share the existing paired
source predicate across both GRUB providers, requiring native MBR/NTFS, retained
valid ISO metadata and explicit UEFI:NTFS. Decoder reconstructs the same choice
and rejects source-free, missing/duplicate source/core, invalid geometry/serial,
customized/enhanced/unknown-field and incompatible mode requests. Image-owned
grldr is never replaced with a standalone loader; source versions stay unknown.
Plain BIOS/FAT routes and standalone contracts remain unchanged.

Remove the old source checker blanket combined-NTFS exclusion: the shared
GetExtraPartitions result already enforces exact pairing. An EFI image on NTFS
requires its payload; a BIOS-only image refuses a gratuitous payload. Validate
the image-owned x64 fallback with the existing bounded reader and retain the
existing source/descriptor/target-before-write checks. PlanFormatPartition and
the unchanged helper already stage/write the original bundled UEFI:NTFS image,
original GRUB4DOS MBR/SBR/standard NTFS PBR, native NTFS format and ISO extraction
through one complete job. Two production files change; 693 of 695 hashes remain
unchanged. No Windows/resource/native-helper/Qt-selector modification. Combined
NTFS remains hidden in the GUI pending native/GUI qualification.

Extend the existing request and source matrices to both GRUB providers rather
than adding a separate test framework. Round-trip the paired operation/source/
filesystem/geometry and verify shared extra-partition planning and provider MBR.
Reject FAT pairing, source-free or supplied-core variants, missing source,
Windows/enhanced/unknown fields, stale metadata and non-MBR. A paired GRUB4DOS
process fixture completes all image phases/bytes with one simulated authorization
and refuses a supplied replacement core without another launch. Initial normal/
sanitizer request runs each fail one assertion after that valid launch: the
old end-of-test expectation remains zero. Correct it to the intended one launch
and rerun the full request suite, preserving both failed logs. No production
failure or formatter change is involved.

Reuse ISO/UDF source/request EFI-presence combinations for both providers,
checking successful BIOS-only and correctly paired EFI requests, and missing/
unnecessary payload refusal. Check EFI read failure/cancellation, vendor-only
and malformed fallback applications. Accepted GRUB4DOS keeps its own loader and
unknown version, bounded layout/payload extent and retained source EFI path;
NTFS keeps no FAT per-file limit. Existing FAT16/FAT32 combined source regressions
remain. Normal and ASan/UBSan/leak-detection runs each pass all 1458 source and
305 request cases, with no final failures/skips or sanitizer diagnostics. Build
the unchanged native helper in both variants. This is contract/backend admission,
not native NTFS paired creation/firmware proof. No physical target or additional
Windows customization installation test. Evidence:
/home/marius/proj/lufus/.rufus-test-artifacts/cp22n69.

Next CP22n70: qualify source-owned GRUB4DOS NTFS paired creation with kernel ntfs
and explicit lowntfs-3g fallback. Extend the existing generated image/readback/
BIOS/UEFI oracles to verify original GRUB4DOS/NTFS records, exact bundled trailing
UEFI:NTFS payload/layout and every ISO-owned file; require actual BIOS menu and
payload-to-source x64 UEFI handoff, unchanged snapshot media, one authorization,
held provider/launcher and durable joined cleanup. Include sanitizers and FAT
regression; keep combined NTFS Qt admission gated until native proof, then test
the ordinary GUI. Other architectures/OS/Secure Boot, real host polkit, physical
hardware and delivery remain required. Keep the single-customization test limit.


CP22n69 cleanup: verify 38 compact evidence files, 25 source hashes and eight
final binary hashes; remove completed normal/sanitizer builds and two temporary
aliases, freeing 399.4 MiB allocated. No checkpoint process remains. Dependencies/
downloads and the user ISO are preserved. Proceed with CP22n70 native GRUB4DOS
NTFS UEFI:NTFS paired kernel/fallback/readback/BIOS/UEFI qualification.


## CP22n70 — native GRUB4DOS NTFS UEFI:NTFS BIOS/UEFI proof (2026-10-07)

Extend the existing GRUB4DOS generated-image recipe to pair NTFS with the original
bundled UEFI:NTFS payload. Native requests follow CP22n69's paired operation;
reuse the unchanged helper, native formatter/providers, original GRUB4DOS MBR/
SBR/standard NTFS PBR and ISO extraction. Combined NTFS GUI companion remains
refused before output creation. No production file changes: all 695 hashes stay
unchanged. No new installer, reader library, provider, protocol or Windows change.

Reuse kernel ntfs and explicit lowntfs-3g with the same foreground-provider/
launcher ownership audits. One simulated authorization covers seven native
phases, monotonic exact copy progress, full device/filesystem sync/private
unmount and held provider/launcher exit before joined completion. Require claim/
descriptor/descendant release, removed private mounts/staging, unchanged ISO,
source-owned original loader/fallback and no standalone cache/core. Fallback
proves absent kernel ntfs before using the real packaged foreground provider.
Run emulator jobs individually using retained Arch 7.2.8 for kernel ntfs and
6.18.55 for fallback; no timing/deadline change or failed attempt this checkpoint.

Independent readback checks original MBR/SBR/gap/NTFS primary/backup records,
0x07 main table entry, formatter-owned serial, label RUFUS_TEST, clean NTFS,
4096-byte clusters, offset 1048576/main size 64962560, exact five source files
and 333310-byte copy total through both 7z and the bounded NTFS-3G reader.
Verify original trailing payload bytes/hash at offset 66028032, track-rounded
partition size 1064448, alongside independent shared-table geometry. Leaf-only
7z records exclude directory entries while every expected nested file is checked.
Add only the exact EFI/BOOT/BOOTX64.EFI path to read-only test population guards;
mutation/traversal remain refused. Read-only nested inode close needs no parent
FILE_NAME update; existing root mutation close retains its parent cache rule.
Bound missing-grldr control mutation to the independently checked main extent,
preserving the trailing payload instead of copying the entire disk remainder.

Normal and ASan/UBSan/leak-detection paired jobs pass with both providers: actual
SeaBIOS original loader/menu CRC/variable/marker execution, original missing-
loader rejection and OVMF bundled UEFI:NTFS v2.8 (x64) banner followed by source
EFI application's shared marker/emulator status33. Both firmware snapshot
backing disks stay unchanged. The existing direct FAT32 combined route remains
a passing native regression with no UEFI:NTFS. Inspect sanitizer NTFS menu and
payload/application serial proof. This is representative firmware proof, not
Windows customization/OS chainload, Secure Boot, other architecture, real host
polkit or physical-device qualification. All five native jobs, eight reader
path/operation guards, Python syntax, five embedded Bash scripts, guest syntax
and five invalid recipe refusals before output creation pass. Evidence:
/home/marius/proj/lufus/.rufus-test-artifacts/cp22n70.

Next CP22n71: admit qualified combined NTFS in ordinary Qt GRUB4DOS selection,
using shared required-extra-partition policy and the paired request operation.
Update existing helper fixture/component rows for frozen source/payload geometry,
original NTFS ISO progress, failures/cancellation and durable joined READY. Then
qualify ordinary native Qt creation/readback/BIOS/UEFI with kernel ntfs and owned
lowntfs-3g, including held provider/launcher and one completion notification,
with sanitizer and FAT regressions. Preserve source ownership/unknown version,
GPT/plain BIOS behavior and one authorization. Remaining architecture/OS/Secure
Boot, real host authorization, physical hardware and delivery work remain
required; keep the single-customization qualification limit.


CP22n70 cleanup: verify 286 compact evidence files, 32 source hashes and 14 final
binary hashes, retaining EFI fixtures/payload hashes/firmware manifests; remove
completed builds/guest outputs and three temporary aliases, freeing 483.3 MiB
allocated. Native recipes already removed disposable disks/guest roots. No
checkpoint process remains. Dependencies/downloads and the user ISO are preserved.
Proceed with CP22n71 ordinary Qt paired GRUB4DOS NTFS admission/qualification.


## CP22n71 — ordinary Qt paired GRUB4DOS NTFS (2026-10-18)

Admit source-owned GRUB4DOS images on MBR/NTFS with their required trailing
UEFI:NTFS payload in the ordinary Qt flow. Remove the two EFI/NTFS selector
exclusions after CP22n69/70 backend and firmware qualification. Reuse shared
filesystem/cluster/extra-partition policy, the existing paired operation and
single helper. Preserve non-hybrid GPT defaults, small-device FAT16 defaults,
manual FAT/NTFS choices across rescans, ISO/DD policy, source-owned loader and
unknown source version. An EFI image whose large-file flag excludes FAT can
still select the paired MBR/NTFS route. No new installer, provider, protocol field,
Windows change or substituted loader. Of 695 recorded production files, only
linux/qt/selection_controller.cpp changes; the other 694 hashes match.

Extend the existing helper fixture to require the paired GRUB4DOS operation.
Normal focused Widgets runs pass 197 cases and ten final selection cases;
the complete final sanitizer selection/creation/failure/standalone subset passes
198. Both builds pass 305 request, 461 helper lifecycle and 77 window cases;
ten retained-source checks also pass normally. Cover 4/8-KiB NTFS requests,
required payload geometry, original ISO NTFS percentages, denial, cancellation,
source replacement/removal, short copy/format, wrong layout, skipped phase,
extraction/sync/cleanup failures, held helper exit and one notification after full
READY. ASan/UBSan and leak checks remain enabled. Initial sandbox-only private
D-Bus and leak-scan refusals are retained separately from passing permitted runs.

Native Qt initially times out before formatting because its minimal initramfs
publishes only the main partition's udev-style link. Publish both ordinary links
for paired NTFS, matching the existing native helper guest. Capture the real
helper's output in this GUI fixture without changing production output or the
held-launcher boundary. An NTFS negative-control timeout also prompts an audit
of the pinned NTFS-3G deletion API: it consumes both child and parent inodes.
Clear both fixture handles after deletion instead of closing the parent again.
The matching upstream implementation/reference hash, six create/delete/absence
cycles and eight path/operation guards are retained. Subsequent host-side stalls
are traced to fsync/ntfs_umount in F2FS rq_qos_wait, not to a guest writer phase.
Retain failed attempts, direct captured-image repetitions and diagnostic traces;
no firmware/command deadline or readback condition is relaxed.

Kernel ntfs and owned lowntfs-3g normal/sanitizer jobs plus combined FAT32 control
pass ordinary Qt creation, independent five-file/readers/record/layout/payload
checks, original BIOS loader/menu execution, missing-grldr rejection and actual
source x64 EFI application handoff. NTFS uses offset 1048576, main size 64962560,
4096-byte clusters and the original payload at 66028032/1064448; copied source
bytes total 333310. FAT32 retains one main volume and direct EFI boot. Each job
models one authorization, holds the provider/launcher across cleanup, waits at
64224/65535 NTFS (64879 FAT) finalization before joined full READY, restores
resources/descriptors, leaves sources and firmware snapshot media unchanged and
emits one completion notification with stable completed unplug. Real host polkit,
physical media, other architectures/Secure Boot and arbitrary OS/loader versions
remain separate qualification work. No additional Windows customization install.
Evidence: /home/marius/proj/lufus/.rufus-test-artifacts/cp22n71.


Final ordinary sanitizer recipes for both providers also pass from RAM-backed
/tmp scratch with the original commands/deadlines and no observer or debugger.
This isolates the diagnosed host-output-filesystem writeback stall; native guest
sync, private unmount, foreground-provider exit and all missing-loader/firmware
checks remain required. The final NTFS BIOS menu and bundled-payload/source-EFI
serial proof are inspected. Both application variants build successfully.

Next CP23a: audit the Windows checksum workflow and add its read-only native
foundation. Preserve default MD5/SHA-1/SHA-256, optional SHA-512 through Alt+H,
original progress/cancellation and source identity. Audit shared-core reuse and
existing native providers before choosing the adapter; qualify known vectors,
streaming/error/cancellation and changed sources without a USB or authorization.
Keep the currently disabled checksum UI gated until its worker/dialog lifecycle
is qualified. Continue the full parity backlog and the single-customization
installation-test limit.


CP22n71 cleanup: compact logs, manifests, source/binary hashes, failed attempts,
readback reports and inspected firmware screenshots are exported. Native recipes
remove guest roots/disks; captured mutation images and remaining firmware/scratch
copies are removed after diagnosis. Dependencies and user sources are preserved.
Retain only the configured normal/sanitizer build directories for the next
planned CP23a checksum-foundation tests and current GUI/helper review; no large
native fixture remains. Proceed with the checksum reuse/Windows-behavior audit.


## CP23a — shared image-checksum core and native reader (2026-10-18)

Audit the original `src/hash.c` and `src/rufus.c` dispatch. The hash toolbar is
available for an idle image selection, independently of USB eligibility. Windows
computes MD5, SHA-1 and SHA-256 by default; Alt+H toggles the persisted optional
SHA-512 calculation. Its result dialog uses the image basename, selectable
monospaced lowercase hexadecimal fields, and an Alt+H hint for omitted SHA-512.
Hashing disables other operations, retains cancellation and reports original
OP_NOOP_WITH_TASKBAR / MSG_271 byte progress. Windows closes its reader and algorithm workers, posts completion, then opens
the dialog from HashThread; that orchestration thread exits after the dialog.
The Linux dialog must run on the GUI thread after joining its read worker, as
required by Qt widget ownership. The Windows async reader uses three 64-KiB buffers and per-algorithm threads.
Those UI, settings and lifecycle conditions remain required for Linux parity.

Reuse audit: the original MD5/SHA implementation, context layout, callback tables
and CPU detection are already suitable for sharing. Extract the algorithm body
byte-for-byte into `src/hash_core.inc` and its declarations into `hash_core.h`;
Windows includes the same body and keeps its existing I/O, threads, dialog,
initialization, PE/AuthentiCode and MD5-list validation. Add both shared files to
Autotools distribution lists and Visual Studio project inventories. Qt and
OpenSSL are existing native alternatives, but a second digest implementation is
unnecessary here; the original core also retains MD5 independently of system
crypto-provider restrictions. No new crypto provider or dependency is added.

The native host shim supplies intrinsics, prefetch and memcpy-based endian
loads/stores, allowing unaligned streaming buffers without the Windows shim's C
alignment casts. Algorithms and constants are unchanged. CPU acceleration is
initialized with pthread_once before native hashing. Reject a big-endian build
until that original little-endian assumption is qualified. This checkpoint proves
x86-64 software and emulated SHA instruction paths; other hosts remain unqualified.

Add `HashImageFileSource`: one caller-owned ordinary source, 64-KiB bounded pread
I/O, independent digest contexts, default three digests and optional SHA-512.
Validate retained descriptor and pathname dev/inode/size/mtime/ctime both before
reading and before publishing. Leave descriptor ownership and file offset with
the caller. Retry EINTR, accept short reads, fail on premature EOF/I/O errors or
changed sources, and clear results on any failure/cancellation. Preserve original
progress identity, withholding the successful endpoint until final validation.
The Linux change detector is needed because POSIX opens cannot impose Windows'
mandatory writer exclusion; it detects changes and is not a snapshot against a
hostile writer. No USB, target selection, authorization, mount or write is involved.
This foundation performs the algorithms in one reader thread; Windows' parallel
pipeline remains a performance-comparison item for worker integration.

Verification: normal and ASan/UBSan/leak builds pass the retained-source test
(161 checks), the native reader contract (62,673 assertions across default/extra
fault cases and guards), and 96 independent hashlib/vector/stream cases per build.
Cover literal abc digests, empty/million-a inputs, padding/block/native-buffer
boundaries, unaligned buffers and 1/3/odd-sized updates. Native cases cover short
reads, repeated EINTR, initial/final I/O/cancellation, changed contents with restored
mtime, replacement/removal/truncation/symlink retargeting, unsupported descriptors,
unchanged offsets and descriptor counts, and a sparse >4-GiB total cancelled before
payload I/O. Four simultaneous first-use readers exercise CPU initialization and
independent contexts. The same 96 comparisons pass under qemu-x86_64 -cpu max,
with SHA-1/SHA-256 acceleration explicitly required and observed. The real host
has neither SHA instruction extension. Both application variants build successfully.
No changes are made to the disabled Qt hash control.

Initial test attempts retain two harness issues: the reused build had not built
the retained-source executable, and removal was expected to return ENOENT even
though its retained inode's changed ctime correctly yields ESTALE first. Build
the target and correct the test expectation; no production validation is relaxed.
The sandbox blocks LeakSanitizer's process inspection, so permitted unprivileged
runs outside that sandbox pass with leak checks enabled. Source comparison proves
the extracted algorithms/declarations and all remaining Windows hash code are
unchanged. No Windows toolchain/application execution is claimed.

Evidence: `.rufus-test-artifacts/cp23a` includes baseline/after snapshots, a scoped
patch, source/core/binary hashes, Windows behavior/reuse audit and normal,
sanitizer and accelerated-path logs. Temporary vector/source files and sparse
fixtures are removed by their tests. Retain the existing CP22n71 normal/sanitizer
build directories for CP23b and current application review; no new large fixture
or physical device is used. Preserve user sources and the completed Mac recovery USB.

Next CP23b: add the read-only Qt checksum worker and its tagged result/progress,
source-freezing and cancellation/join contract, then qualify UI eligibility,
exclusive operation state, result dialog, Alt+H and persisted settings before
enabling the toolbar. Continue the remaining full parity backlog and the single
Windows customization installation-test limit.


## CP23b — Qt image checksums, result dialog and Alt+H (2026-10-18)

Enable the ordinary checksum toolbar for an accepted, idle image selection,
including raw .img and ISO/UDF sources. No USB, discovery service, preparation
helper or authorization is needed. Default MD5/SHA1/SHA256 and optional SHA512
use CP23a's unchanged original core. Alt+H toggles the original EnableExtraHashes
preference (default false), with status feedback. Freeze that choice and the
accepted source identity in ImageHashJob before its worker starts. Raw selections
now capture the same existing source identity used by analyzed images.

The job reuses RufusJob's reporting/serialization/cancellation contract,
OpenImageFileSource, CheckPrepareSourceIdentity and HashImageFileSource. Tagged
progress and results feed a dedicated ImageHashController. A received result stays
private until QThread completion and wait() have joined cleanup. Cancellation
invalidates pending success, keeps other operations disabled through exit and
can be followed by a fresh request. Destruction requests interruption and joins
without requiring queued GUI delivery. Failures and exceptions publish no hashes.
Selection and settings mutators use the common busy guard; scans, checks,
authorization, confirmation and writes exclude checksum starts. Discovery still
updates while hashing; a first device arriving during work is selected only after
exit. Removal/insertion does not cancel a file hash or start a write.

Present results on the GUI thread after joining, with the original source basename
as title, read-only monospaced selectable MD5/SHA1/SHA256/SHA512 fields, original
omitted-SHA512 Alt+H hint, default OK and Escape. Long digests wrap without adding
copyable newlines. Fields follow the application font size and available screen
width; narrow/large-font fields retain scrolling and full-value copy. Image names
and errors remain plain text. Hashing starts/stops the elapsed timer and uses the
original byte progress, bounded below full READY until joined success. Success
shows full green READY and one beep/alert, as Windows UM_FORMAT_COMPLETED also
does for HashThread. Errors show FAILED and an error dialog; Cancel restores an
idle window with CANCELLED and no results or data-loss confirmation. Source
selection and explicit settings survive cancellation. Hash state is reset by a
new image, boot choice or write.

Original Windows orchestration differs only where Qt ownership requires it:
HashThread closes its reader/algorithm workers, posts completion and opens its
modal dialog before the orchestration thread exits. Linux joins the read worker,
then opens the dialog on the GUI thread. POSIX source change detection remains
CP23a's documented alternative to mandatory Windows share exclusion. A cancelled
read-only hash uses the existing Linux cancellation status instead of an extra
informational modal notification; cancellation is already requested explicitly
and no device operation has occurred. Per-algorithm parallelism remains a measured
performance follow-up; this checkpoint does not remove that Windows pipeline
from scope.

Normal and ASan/UBSan/leak runs pass 14 checksum-widget cases, three scaled
large-font cases, 46 image-analysis cases, 77 ordinary window cases and three
scaled window cases. The checksum fixture independently compares digest strings,
checks raw/ISO acceptance and analysis gating, no-target operation, frozen optional
hashes/settings, direct mutator/shortcut guards, source replacement before opening
and before reading, I/O errors and exceptions, queued-success cancellation,
cleanup-held progress/notification ordering, retry, teardown with no GUI events,
USB refresh/deferred first selection, literal filenames, clipboard bytes, OK/Escape
and large application font at 1x/2x scaling. Worker UID stays equal to the caller;
any pkexec launch aborts the fixture. Simulated raw creation, durable READY, retry
and declined-confirmation regressions also pass in both builds (six cases per
run including initialization/cleanup). Both application variants build.

Screenshot review catches an initially ineffective large-font test (changing the
parent window does not change a new top-level dialog's application font), then
horizontal scrollbars stealing space from wrapped rows. Set the actual application
font in the fixture, inherit its size in the monospaced fields, bound width to the
screen and disable horizontal scrolling for wrapped edits. Assert default digest
rows need no vertical scrolling and inspect default/extra and scaled large-font
captures. No production checksum algorithm, reader, helper or protocol changes.
Sanitizer process inspection and private D-Bus regression fixtures run unprivileged
outside the sandbox's known ptrace/socket restrictions; leak checks remain enabled.
No physical disk, Windows install or additional customization qualification is used.

Evidence: `.rufus-test-artifacts/cp23b` retains before/after snapshots, scoped patch,
source/binary and shared-core hashes, normal/sanitizer/build/regression logs,
inspected screenshots and cleanup inventory. Temporary source trees/ISOs/settings
are removed. Retain the CP22n71 normal/sanitizer builds for current app review and
the next checksum pipeline measurement; preserve user sources and the Mac USB.

Next CP23c: measure the native checksum reader against Windows' three-buffer,
per-algorithm pipeline with representative generated inputs. Reuse the shared
algorithms and qualify a native parallel pipeline if needed to preserve its
throughput, bounded memory, cancellation and source/result/READY contracts. Keep
all remaining parity groups in scope and retain the single-customization
installation-test limit.


## CP23c — bounded native parallel checksum pipeline (2026-10-18)

Revalidate CP23b as completed progress and audit the current Windows HashThread,
IndividualHashThread and SetThreadAffinity implementation. Windows uses three
64-KiB read buffers, one worker per requested algorithm and asynchronous reading.
The first Linux reader hashes algorithms sequentially in its read worker. Measure
that exact retained source and a native pipeline with the same unchanged digest
core, compiler and optimized flags before claiming a performance improvement.
No actual Windows executable is benchmarked or qualified here.

Replace only the native file reader's internals. One pread thread feeds three
64-KiB slots; three or four algorithm workers each retain their own context and
consume every slot in source order. A slot remains immutable until all consumers
release it. Fast algorithms can proceed within that bounded queue while a slow
consumer applies backpressure. The calling RufusJob thread coordinates progress,
cancellation, final source checking and result publication. No Qt cancellation or
reporting query runs on a pthread reader/consumer. Completed byte counts mean all
algorithms have consumed those bytes; read-ahead alone is not progress. Short reads
are accumulated into the current slot; EINTR is retried and premature EOF fails.

Use monotonic condition waits with 50-ms cancellation polling. On failure/cancel,
wake every pipeline wait and join every successfully created reader/algorithm
thread before freeing buffers or returning. Treat partial allocation/mutex/
condition/attribute/thread setup as errno failures with empty results. Keep the
source descriptor and offset with the caller. Source dev/inode/size/mtime/ctime
must still match after all joins. Allocate the over-aligned contexts and buffers
with posix_memalign; total pipeline allocation stays below 256 KiB regardless of
image size, with a fixed maximum of five pthread workers. Join/teardown invariants
never allow a result or freed shared buffer while a worker remains outstanding.
The existing GUI source-close/QThread-join gate still follows the native pipeline.

The native scheduler honors the caller's Linux CPU affinity/cpuset instead of
installing Windows affinity-mask groups. A single-allowed-CPU run also passes.
Cooperative cleanup replaces Windows' forced thread termination: an in-flight
regular-file read must return before its reader can be joined. Full READY/results
remain gated during that cleanup. This preserves the port's completed-I/O contract
and avoids abandoning a thread with access to freed buffers or a closed source.
Algorithms, default/optional hashes, CPU detection, GUI/settings, helper and wire
protocol are unchanged; no additional dependency or physical target is involved.

Optimized standalone comparisons use GCC 16.2.1, -O2/-DNDEBUG, the same probe/core/
source adapters, generated nonzero RAM-backed files and three samples per size/
mode, alternating variant order. Each run independently verifies all requested
digests with hashlib and checks source inode/size/mtime/ctime. Median MiB/s:

| Input | Hashes | Retained serial | Native pipeline | Ratio |
| --- | --- | ---: | ---: | ---: |
| 64 MiB | MD5/SHA1/SHA256 | 103.10 | 200.36 | 1.94x |
| 64 MiB | plus SHA512 | 80.49 | 209.53 | 2.60x |
| 256 MiB | MD5/SHA1/SHA256 | 112.45 | 220.09 | 1.96x |
| 256 MiB | plus SHA512 | 81.97 | 215.99 | 2.63x |

This quantifies the native algorithm/read pipeline on this shared x86-64 host,
without SHA instruction support, with warm memory-backed input and no-op progress
callbacks. It is not a Windows application, storage-device or complete desktop
throughput comparison. Both configured normal/sanitizer GUI/helper builds remain
Debug; optimized probes are separate compact evidence binaries.

Normal and ASan/UBSan/leak checks pass the retained-source and reader fault suites,
27 new startup/ownership/backpressure/cancellation cases, 100 independent digest/
stream cases, 14 GUI cases and three scaled GUI cases. Reader-fault fixture
counters/cancellation become atomic because pread now runs on a reader pthread;
assertions still check each short read, error, source mutation, offset and empty
failed result. Progress may coalesce, so assertion counts depend on scheduling.
The new test holds all algorithms, proves exactly three slots of read-ahead,
releases fast consumers while SHA256 is held, verifies bounded reuse with literal
independent digests, and cancels while the reader is blocked and consumers remain
held. Completion checks require all thread joins and destroyed resources. Inject
allocation, mutex, condition attributes/clock, each condition init, and each default/
extra algorithm or reader creation failure. Thread ownership is checked for
reporting/cancellation and source I/O; the source offset stays unchanged.

ThreadSanitizer also passes the 27 native lifecycle cases. They pass when confined
to one allowed CPU. QEMU's SHA-capable CPU executes the same 100 vector/stream
comparisons with SHA1/SHA256 acceleration explicitly observed. Both application
variants build. Leak checks run unprivileged outside the known sandbox inspection
restriction; ThreadSanitizer succeeds inside the sandbox. No checks, timeouts or
source/cleanup gates are relaxed. The original core and Windows implementation
hashes remain unchanged; original Windows application execution stays deferred.

Evidence: `.rufus-test-artifacts/cp23c` contains retained baseline source/probe,
optimized comparison script/data/binaries, standalone build recipes, before/after
snapshots and scoped patch, source/binary/core hashes, normal/sanitizer/TSan/single-
CPU/emulated logs and cleanup inventory. Generated 64/256-MiB benchmark files and
all component/GUI fixtures are removed. Retain the existing normal/sanitizer builds
for current application review and the next parity work; preserve user sources
and the completed Mac recovery USB. No additional Windows customization install.

Next CP24a: audit compressed-image detection/decompressed-size/boot-marker rules
in src/vhd.c, src/bled and Win32 selection/format dispatch. Assess reusable bundled
bled/native providers and add the read-only compressed-source foundation before
extending SELECT/DD/helper/UI contracts. Keep compressed inputs, VHD/VHDX/FFU and
the remaining full parity backlog in scope; do not bypass target sizing, source
identity, one-authorization, sync/cleanup or joined READY requirements.


## CP24a — shared compressed-image policy and gzip source (2026-10-18)

Audit Windows `IsCompressedBootableImage`, Bled's size/preview/full decode, the
format dispatch and sector adapter. Extract the exact original compression enum,
FFU/VHD/VHDX identifiers and extension table into portable shared headers. Windows
and Linux use the same filename and strict 512-byte marker policy. Keep all eleven
original associations, including case variants and the original path-length guard;
the reserved .7z identifier does not become a new offered association. Update both
Windows project/filter inventories and Autotools distribution lists. Windows
backend calls, preview sizing, FFU/VHD mounting and the remaining vhd.c code retain
their behavior. Mechanical extraction checks pass; original Windows execution
remains deferred.

Add a read-only native gzip source provider using a retained regular descriptor
and pread. Input/output buffers are 64 KiB each; provider allocations have a
256-KiB total per-operation budget and tracked cleanup even during initialization
failure. There is no expanded temporary file. Require an explicit output bound,
count decoded bytes in 64 bits, validate CRC/ISIZE for every contiguous gzip
member, retain the first sector and report original ignored-tail behavior. The
optional consumer receives ordered provisional bytes; all failed/cancelled results
are empty. Retry EINTR/short reads, reject premature EOF, propagate consumer
errors, preserve source ownership/offset, and recheck retained/path identity before
publishing. The caller still decides media geometry and bootability.

Concrete native differences: original Bled depends on Windows handles/CRT aliases,
global callback/cancellation state and nonlocal errors whose allocation ownership
is not uniformly recoverable. Use zlib's per-stream allocation/error interface for
bounded, cancellable cleanup. Original full gzip already validates CRC/ISIZE; this
is preserved, not claimed as a new algorithm feature. Original prefix selection
stops after 512 bytes and projected size may use a modulo-2^32 gzip footer or an
untrusted partition hint. The native foundation validates the entire stream and
counts exact output before later target sizing, to avoid undersized destructive
jobs and late corruption discovery. It also rejects reserved gzip flag bits, bad
optional header CRCs and a bad first-member ISIZE followed by a valid member; the
original does not validate those flags/header checksum and can obscure the first
size error while adding later output. Invalid source checksums/headers must remain
failures. It therefore does more source I/O. The
original Windows Bled algorithms are unchanged. Other original formats currently
return ENOTSUP here, pending their own provider/format qualification. Windows'
Unix-compress magic fallback inside .gz also returns ENOTSUP pending LZW support.

Normal and ASan/UBSan/leak runs pass 134 shared-policy checks, 949 native contract
checks, 71 independent corpus/large-stream cases and 161 retained-source regression
checks. The native contract also passes ThreadSanitizer, including three concurrent
readers sharing a descriptor. Test malformed headers/trailers, optional fields,
concatenation across input boundaries, missing markers, limits, read/allocation/
consumer faults, cancellation and source mutation/replacement/removal. A real
gzip stream expands to 2^32 + 512 bytes while its footer advertises only 512;
exact-size success and a 2^32 cap failure both pass without storing expanded data.
Both normal/sanitizer apps build. Only existing upstream wimlib build warnings
are observed. Leak checks run unprivileged outside the sandbox inspection limit.

Evidence: `.rufus-test-artifacts/cp24a`, including retained source, scoped patch,
source/binary/provider hashes, reproducible Windows extraction checks (22 checks;
62 Bled implementation files unchanged), logs and cleanup. Temporary corpus and
large compressed streams are removed. Retain the existing normal/sanitizer builds
for the compressed writer/helper checkpoint. Preserve user sources and the Mac
recovery USB. No physical target or extra Windows customization install is used.

Next CP24b: add gzip decoded-image sizing and a sector-aware native write adapter
on disposable regular files. Freeze exact decoded size/source identity; safely
round target capacity, zero-pad/check a final partial sector, and preserve write,
sync, cancellation and cleanup errors. Then qualify the helper/wire and ordinary
Qt admission before exposing compressed creation. Keep all remaining compressed,
container, advanced and legacy parity work in scope.


## CP24b — exact decoded plans and sector-aware gzip file writer (2026-10-18)

Revalidate CP24a's authoritative production hashes and passing evidence as progress.
Audit original `src/format.c:sector_write` and compressed WriteDrive dispatch, plus
the native raw writer and target inspector. Original compressed DD accepts stream
chunks outside sector boundaries and buffers the final sector. Its final partial
WriteFile result is deliberately unchecked and padding is not explicitly cleared.
Native completion must not hide a final-sector failure or emit stale padding.
Keep Windows code unchanged and share its marker/extension rules, original digest
core, native target inspector and RAW_WRITE_RESULT phase/count contract.

Add `CompressedImageWriteSize`, `PrepareCompressedWrite` and
`WriteCompressedImageToFile` in `linux/platform/compressed_write.c/.h`. Plan the
complete retained gzip source with an explicit output bound and sector size;
freeze inode/content identity, exact decoded length, prefix/members/ignored tail,
SHA256 and the explicit marker-force choice. Require a full prefix even when
forced. Round to 512..65536-byte power-of-two sectors without overflowing signed
64-bit offsets. Bad/cancelled planning clears the complete result. No target is
opened or authorized by planning.

The writer borrows an existing buffered O_RDWR regular file and rejects source
aliases, incompatible flags, non-regular targets and insufficient padded capacity.
Recompute the entire frozen plan before the first write; altered digest, decoded
length or stream facts cannot permit destructive work. Recheck target/source after
preflight. Decode into bounded 64-KiB batches, including tiny concatenated members;
hold the pending tail until complete stream/source/fingerprint validation, then
explicitly zero-pad/check its final sector. Retry short I/O/EINTR using pread/
pwrite, preserve seek positions, file length and bytes beyond padded output.
Flush any written bytes even after a copy error or cancellation, retaining both
the primary error and a separate sync_error. Verification decodes again and
compares every padded byte, including zeros, then checks source and target again.
DONE follows those gates; callers still own close/helper/worker/READY gates.
Readback can use cache and is not a persistence or boot proof.

The original gzip full-stream validity/tail behavior and CP24a's documented strict
header/checksum differences remain. Complete preflight and explicit padding/write
checks are intentional native correctness differences required by safe capacity
planning and joined READY. A writer call makes three full decode passes (preflight,
copy, readback), in addition to initial plan construction; no expanded temporary
image is stored. Buffers and the existing decoder allocation budget remain bounded.
Per-call state has no shared mutable decoder or plan. Move the existing native
CPU dispatch pthread_once into `hash_core.c` with `hash_native.h`, and have both
the unchanged checksum pipeline and new planner call it before hashing. This avoids
concurrent initializations writing shared dispatch flags. Original shared core and
Windows startup/algorithms remain byte-identical.

Normal and ASan/UBSan/leak checks pass nine focused suites: 230 new geometry/plan/
write/flush/readback cases, 39 raw-write regressions, 134 compression-policy checks,
949 compressed-source checks, 71 corpus/large-stream cases, 161 retained-source
checks, native checksum faults, 27 pipeline lifecycle cases and 100 digest vectors.
The complete 230-case writer suite also passes ThreadSanitizer, including concurrent
first use by two planners and a checksum pipeline on one retained descriptor.
QEMU with SHA instructions passes all 100 digest vectors. Both normal/sanitizer
apps build; only retained upstream wimlib warnings occur. Leak checks run as the
unprivileged caller outside the sandbox inspection restriction. TSan runs inside
the sandbox and does not instrument the distribution zlib DSO.

Evidence: `.rufus-test-artifacts/cp24b` contains before/after snapshots, scoped diff,
production/algorithm hashes, init extraction proof, reproduction recipes, logs,
independent case descriptions and cleanup. All disposable sources/targets and large
compressed streams are removed. Retain existing builds for compressed device/helper
integration and current app review; retain only a compact TSan probe in this new
checkpoint. Preserve user sources and the Mac recovery USB. No physical media,
privilege prompt, package install or additional Windows customization installation.
File sector emulation/synthetic marker data is not a compressed OS boot proof.

Next CP24c: qualify a block-descriptor compressed writer and integrate exact decoded
identity/geometry/digest/force into the single authorized helper operation and wire
validation, with focused request/fault/cleanup evidence before ordinary Qt admission.
Keep other compression providers (including Unix-compress fallback), VHD/VHDX/FFU,
advanced/legacy features and the complete parity backlog in scope.


## CP24c — gzip block boundary and authorized helper/wire integration (2026-10-18)

Rehash all 718 CP24b production files before editing; they match the preceding
progress checkpoint. Share the same compressed copy/sync/readback engine between
regular files and a new authorized-block entry point. The block boundary uses the
existing target inspector, requires expected dev_t/capacity/logical sector before
writing and after sync/readback, rejects a direct source-device alias and checks
padded capacity. The owner still supplies whole-disk ancestry, holders/mounts and
an exclusive claim. The current Raw target already accepts 512..65536-byte sectors;
retain that range. Formatting retains its separate qualified geometry.

Add strict `compressed-write-device-v1` metadata to the existing raw job protocol:
stored source identity plus decoded byte count, logical sector, SHA256 and explicit
ignore-marker choice. Preserve the old raw packet shape. Reject missing/unknown/
mistyped/noncanonical metadata, unsupported types, wrong suffixes, mixed jobs,
short output, overflow and padded capacity mismatch. Canonical decimal strings
retain 64-bit sizes; hashes are exact canonical lowercase hex. Move the unchanged
pure geometry function to its own object, avoiding source/decoder/reporting linkage
for protocol-only users. Progress/results use padded physical bytes; native full
preflight stays within the existing checking phase and emits no illegal PREPARE
raw-progress frame.

The same single authorized helper worker accepts the request. Check caller-owned
readable/no-follow regular source and frozen inode/content identity, reconstruct
the full exact gzip plan and compare decoded size/hash before any target acquisition.
Acquire through existing UDisks/native source/home/mount/holder checks, then call the
new block boundary while retaining the claim. No partitioning, formatting, remount,
eject or second authorization is introduced. Revalidate the target after verified
writing, then explicitly close target/home and source before returning the outcome.
Close once, including EINTR; avoid retrying a released Linux descriptor. A close
failure prevents success and overrides a plain cancellation. Existing noncancel
write errors remain primary. Preserve legacy native raw capacity preflight even
for callers supplying only its documented source/device fields.

Fix a concrete helper completion issue: cancellation EOF previously overwrote
an actual job failure unconditionally. Preserve a reported noncancel failure on
cancellation so a close error cannot become CANCELLED. Keep normal cancellation
and timeout/protocol behavior; sync_error still independently prevents cancellation
from hiding a failed flush. The shared source destructor remains best effort for
other routes; raw/compressed jobs now use its explicit checked close.

Normal and ASan/UBSan/leak runs pass seven focused suites: 230 retained file-writer
cases, 49 native helper/job/kernel-boundary/claim/cleanup cases, 59 Qt raw/compressed
protocol/process results, 39 raw-write cases, source 10, preparation process 66 and
UDisks target 53 QTest results (including framework setup/cleanup). The native
fixture compiles the real helper/worker/source/target/writer and replaces only
authorization, kernel and selection/mount primitives at link time. Real exclusive
file leases are checked through source reads, writes, fsync and verification; all
files are private regular fixtures. It covers 512/4096/65536 sectors, gzip larger
than its decoded target, legacy raw compatibility, source ownership/identity,
decoded digest/size, protected/unmount paths, boundary type/geometry/capacity,
dirty write/cancel/flush errors, readback/padding corruption, kernel fact changes,
final snapshot and checked close/EINTR failures. The Qt process tests cover single
pkexec dispatch and success only after helper exit, including cancellation plus
sync/close failure and stored/decoded totals that omit padding.

Both normal/sanitizer application/helper variants and the retained alternate ISO
target fixture build. Old compressed source/core/Windows algorithms remain unchanged;
no new provider or tool install. The helper now links the selected zlib runtime,
documented for packaging. New wrappers contain no production branch or environment
override. Private D-Bus/leak tests run unprivileged outside the known sandbox limits.
Initial fixture cancellation needed an explicitly forced test progress boundary:
production throttling may retain a 17-byte sample while the test waits. One early
run also began before that fixture finished building; final runs follow completed
builds. No product gate was weakened. Temporary sources/targets are removed.
Two approximately 0.6-MB system-managed core files from initial fixture failures
remain: noninteractive sudo requires a password. Their exact paths/PIDs are recorded
for authorized cleanup; subsequent fixture children disable core dumps without
changing failure detection. The updated harness passes all 49 cases in both builds.

Evidence: `.rufus-test-artifacts/cp24c`, before/after/diff and hashes, engine/geometry
reuse proof, build/test logs, provider/runtime metadata and cleanup. Retain current
builds for real kernel-device qualification; preserve user sources/Mac recovery USB.
This is native integration with simulated kernel/selection facts, not real block
claim, physical-device, compressed OS boot or ordinary GUI qualification. Do not
enable ordinary compressed selection on this evidence alone.

Next CP24d: qualify compressed native/helper I/O, exclusive lifetime and bootable
readback on a disposable guest kernel disk, including failure/cancellation/sync and
cleanup. Then connect frozen compressed analysis/plans and padded progress to
ordinary Qt admission with joined READY. Keep all original providers, Unix-compress
fallback, VHD/VHDX/FFU and remaining full Windows parity work in scope. No additional
Windows customization installation matrix.


## CP24d — guest-kernel gzip helper and bootable readback (2026-10-18)

Rehash all 719 CP24c production files before editing. Extend the existing guarded
USB guest recipe with `--compressed-raw-helper`. Add a VM-only link-fault helper,
compiled from the unchanged production helper, and shared guest case code. Only
the VM target inventory changes in production CMake; all native runtime, original
Windows and shared algorithms in the preceding production inventory stay unchanged.

Normal and ASan/UBSan/leak guest runs each pass 15 gzip jobs after five existing
Syslinux seed and five legacy raw jobs. Use a real 64-MiB USB block disk with
512-byte sectors on kernel 6.18.55-2-cachyos-lts under QEMU TCG. UDisks properties
and the one pkexec launcher are guest fixtures. UID/root authorization inside the
guest, sysfs/ioctl facts, mount/home/source checks, exclusive claims and I/O remain
native. No host disk passthrough, network, polkit, mount or physical USB is used.

Reject decoded digest/size mismatch, actual kernel sector mismatch and gzip CRC
corruption without changing the disk. Real mounted-target cases cover a busy
working directory, caller home on target, EOF cancellation after unmount and
successful gzip creation. Copy 1 MiB + 37 bytes, verify its 475 explicit padding
zeros, and retain physical-byte progress/results. Every gzip job has one launcher;
the client observes a held launcher and joins it before completion.

Six observed/fault jobs retain the real O_EXCL claim through actual writes, dirty
fsync, verification and just before checked close. Cover a real 17-byte short write
then EIO, cancellation after 65536 real bytes, an actual fsync followed by injected
failure, corruption of an actual readback buffer (EBADMSG), a real close followed
by failure, and observed success. Seed distinct target bytes before each job;
independent reads check the written prefix/padding and hash every byte beyond the
actual writes. Dirty failure/cancellation flush once, sync errors remain distinct,
close failure remains FAILED, and claims/descriptors/children are released before
the joined result. Observer state is thread-local; no kernel/UID/backend fact is
substituted in this helper build. The production helper contains no fault control.

Restore the complete bootable 64-MiB image from gzip using the unchanged production
helper. Full guest and independent host hashes agree. Check MBR, FAT and files,
then execute BIOS -> Syslinux/menu -> boot-sector fixture (`RUFUS_RAW_BOOT_OK`) in
a second VM; backing media stays unchanged. This proves a restored bootable image,
not an installed OS or a Linux kernel boot. Other real sector sizes, physical
hardware and host polkit remain unqualified; preceding simulated sector evidence
and the supported 512..65536-byte range remain intact.

Initial attempts found only new fixture issues: incorrect CLI option, an exit
observation after completion, and an EIO expectation where readback mismatch uses
EBADMSG. A parallel sanitizer run hit the legacy 15-second mounted-job fixture
limit. Add timing/phase diagnostics and a bounded compressed-only fixture deadline;
legacy and product deadlines stay unchanged. Final sequential runs pass; sanitizer
mounted gzip success takes 4175 ms, below the old boundary. No product gate or
error contract is weakened. Existing upstream wimlib/libcdio build warnings remain.

Evidence: `.rufus-test-artifacts/cp24d`, including before/after/diff, unchanged
production proof, guest command/input hashes, per-case errors/counters/claims,
actual timings, sanitizer and BIOS serial logs, independent file/media hashes,
recipes and cleanup. Both completed and failed recipe runs remove their owned
disks/root trees/archives. Retain current builds and the small qualified installer
build for the next Qt/guest work. Preserve user sources/Mac recovery USB; no extra
Windows customization install and no retry of the prior root-only core cleanup.

Next: freeze compressed source analysis/decoded identity and chosen target geometry
in the ordinary unprivileged Qt flow; admit the single helper job, padded progress
and joined READY with focused component/guest evidence. Keep other original
providers, Unix-compress fallback, VHD/VHDX/FFU, image saving and the full advanced/
legacy Windows parity backlog in scope. Ordinary compressed selection remains gated.


## CP24e — ordinary gzip selection and confirmed Qt DD creation (2026-10-18)

Revalidate the 719-file CP24d production baseline. Admit gzip in ordinary SELECT
and the existing confirmed DD controller. Analysis runs a complete retained-source
validation on the existing cancellable/joined RufusJob worker, freezing stored
inode/content identity, exact decoded size and SHA256. Set the shared compression/
projected-size/marker report facts; skip ISO/Windows scanning for compressed input.
Reject malformed/truncated/CRC/short/unmarked/unsupported input and changed sources.
Discard cancelled or superseded queued results and join before a replacement.
Keep legacy .img and ISOHybrid behavior and root/helper/backend bodies unchanged.

The original Windows compressed selection uses a provider size hint and a 512-byte
prefix. Native SELECT validates the entire gzip stream to freeze an exact 64-bit
size/hash and CRC before START. This costs a full scan, kept off the GUI thread;
START itself does not decompress. With the existing helper's four passes, the
ordinary flow makes five full decode passes, stores no expanded production image,
and retains bounded decoder/transfer memory. CP24a/b's documented provider and
correctness differences remain; Windows code/algorithms are unchanged.

Read optional logical sector size in the verified native USB facts snapshot;
unknown/invalid remains zero for existing classification. Recheck it with final
identity, preserving the HDD/card scoring inputs. Compressed START requires a
known 512..65536 power-of-two sector, freezes native sequence/sector, rounds exact
decoded bytes and checks padded capacity. Compare fresh UDisks/source/geometry
again after confirmation. Retain accepted source/hash rather than reanalyzing on
the GUI thread. The same helper reconstructs and validates its own plan before
target acquisition, then checks real descriptor geometry. No new authorization,
helper provider, mount, eject or remount. The source scan is target-independent;
its canonical 512-byte analysis geometry never substitutes for selected media.

Transfer frozen compression fields through the existing raw controller and strict
compressed-write-device-v1 packet. Use padded bytes for controller/helper progress,
verification and completion. Confirmation displays the written DD size and stored
compressed size separately. Add *.gz to the picker and a decoded-size status.
Native ownership/source protection stays before the single authorization. READY
and one completion alert still require verified counts, successful cleanup and
helper/launcher exit; copy/result receipt alone cannot produce 100%. Force-marker
UI/other providers remain pending; ordinary gzip uses the unchanged strict marker.

Normal and ASan/UBSan/leak runs pass 55 analysis, 77 native-facts and 45 focused
Widgets QTest results (including setup/cleanup). Cover worker affinity, CRC/marker/
size/unsupported failures, changed source, queued cancellation/replacement, native
sector parsing/change, 512/4096/65536 planning, stored gzip larger than target,
padding overflow/nonaligned capacity/unknown geometry, source/media invalidation,
one authorization, failure/cancellation, 99% held result, full green READY and one
alert after joined exit. Widgets uses private sockets and protocol-only children;
no physical block node is opened. The ordinary decoder/backend is real.

Both disposable guest recipes pass 26 jobs each: CP24d's 25 cases plus ordinary
Qt SELECT -> START -> confirmation -> production helper creation. Start from a
distinct poisoned prefix, restore all 67108864 bytes from gzip, assert source
hash/decoded identity, observed 512-byte geometry, no early authorization, one
launcher and no completion while it is held. Independent guest/Qt/host hashes,
MBR/FAT/files and BIOS Syslinux/menu/boot-sector execution agree. The guest models
UID1000 only in the front-end thread; its exec'd production helper is real root.
Private UDisks properties and pkexec are fixtures. No physical USB, host polkit,
other real sector size or installed-OS/Linux-kernel boot claim is made.

Initial new Widgets fixtures changed disk sequence after discovery, then needed
explicit re-selection after refreshing that snapshot. Production correctly rejected
both stale selections. Correct only the fixtures; final focused runs pass. Initial
broad normal Widgets reports 1455 passed/11 new-fixture failures/5 skips; its broad
sanitizer counterpart reaches the existing 600-second limit. Neither is claimed
as a complete passing suite. The focused current gzip/raw regressions and the two
complete analysis/native-facts suites pass in both builds. No timeout/product gate
was weakened. A rejected escalation due to the earlier stop request was followed
by explicit human resume/test approval. The timeout left no test processes; export
hashes then remove its verified 21.5-MB owned temporary tree. VM recipes remove all
owned disks/archives/trees. Existing upstream wimlib/libcdio warnings remain.

Evidence: .rufus-test-artifacts/cp24e, snapshots/diff, ten scoped production changes,
709 unchanged production hashes, unchanged Windows/decoder/writer/helper/protocol
proof, exact builds/commands, passing focused logs, initial failures/timeout,
normal/sanitizer guest input hashes/serial/counters/readback/boot and cleanup.
Retain current builds and small qualified installer for continued parity work.
Preserve user sources and the Mac recovery USB. No extra Windows customization
installation matrix or root-only core-cleanup retry.

Next: audit and implement the remaining original compression providers (starting
with XZ), then their common Qt/helper admission. Keep Unix-compress fallback,
VHD/VHDX/FFU, force-marker UI, saving and all advanced/legacy parity work in scope.
The complete Windows parity objective remains unfinished.


## CP25a — original XZ decoder reuse and verified retained-source reading (2026-10-18)

Resume after explicit human approval and verify all 719 CP24e production hashes.
Add the read-only XZ provider to the existing retained-source API; keep production
XZ selection/preparation/device admission gated. PrepareCompressedWrite now
explicitly retains gzip-only admission as source capabilities expand. A valid XZ
image returns ENOTSUP before decoding/planning or changing a regular-file target.
Existing protocol/target acquisition/Qt/controller/helper behavior stays unchanged.

Compile the original XZ Embedded LZMA2 and BCJ implementation directly, retaining
its host-enabled BCJ set and 64-MiB dynamic dictionary limit. Use ordinary native
calloc/free, zlib CRC32 and the shared original SHA256 core/once-only CPU dispatch.
No external xz/liblzma is used by production. Separate provider buffer lifetimes
so XZ does not carry unused gzip buffers. Each call uses two 64-KiB I/O buffers,
small local integrity state and at most the original dictionary plus bounded
fixed decoder states; no expanded production file, global mutable decoder or
nonlocal BusyBox error exit. Preserve descriptor ownership/offset and retained
source checks, explicit output bounds, 64-bit actual counts, provisional callbacks,
512-byte prefix, cancellation/cleanup and result clearing on all failures.

Windows unpack_xz_stream ends at the first stream, leaving padding, another stream
or arbitrary trailing data unread as image content. Preserve that behavior and
report trailing bytes; XZ concatenation is not gzip member concatenation. The
original XZ_DEC_ANY_CHECK build verifies CRC32 but silently skips CRC64/SHA256 and
reserved check fields. Native XZ verifies CRC64 and SHA256 per block and refuses
reserved check IDs. This deliberate integrity difference prevents corrupt decoded
images from acquiring a validated native plan. Add four opt-in hook sites to the
shared stream source. With hooks disabled, before/after preprocessed translation
is byte-identical (125202 bytes on this host). Original wrapper, LZMA2/BCJ/config,
hash algorithms, Windows build flags and native helper/protocol remain unchanged;
no Windows executable or other host architecture was run.

Both Debug and ASan/UBSan/leak builds pass all five affected suites: 221 numbered
XZ corpus/differential/large-stream cases; 966/967/970 retained-source checks for
CRC32/CRC64/SHA256; 1029 gzip source checks; 71 gzip corpus/large cases; 230 regular
file geometry/plan/write/sync/readback cases; and 55 complete Qt analysis results.
Tests use independent Python liblzma, xz CLI multi-block fixtures, SHA256/CRC32 and
original-core hook-disabled comparison. Reject each damaged data-check byte and
header/index/footer/truncation, exercise empty streams and explicit empty blocks,
64-KiB I/O boundaries/short reads, 64-MiB dictionary boundary/over-limit, output
bounds, reserved checks and enabled x86 BCJ. Prove original acceptance/native
rejection for damaged CRC64/SHA256. Every provider allocation failure, cancellation,
callback error, source replacement/removal/change and concurrent first use cleans
up and clears results. Default 64-KiB-dictionary contract peak is 95416 heap bytes;
this measured fixture peak is not a claim that every valid stream uses that size.

A hand-assembled single LZMA2 block with sparse uncompressed chunks actually reads
and decodes more than 4 GiB, independent small-form decoding validates its format,
and the large decoder counts 2^32+512 and rejects a 2^32 bound correctly. Logical
source size also exceeds 4 GiB; sparse allocated storage is about 256 MiB and is
removed. No expanded image is stored. The original range-coded large fixture
passed normally but exceeded 90 seconds under sanitizer instrumentation after
all XZ corpus/contract checks passed. Four other sanitizer suites passed. Replace
only that large size-accounting fixture with the valid sparse form, retaining
real decoder I/O/counting and the same bounds. Re-run only XZ in both builds; both
pass. Preserve the timeout and original normal evidence. New XZ test bounds
allow 90 seconds per large read and 180 seconds for the suite; existing gzip/Qt
bounds are unchanged. The original-check probe is test-only and never installed.
Qt/leak tests run unprivileged outside the socket/process-inspection sandbox under
the user's explicit authorization. Both current GUI/helper builds succeed. Initial
normal component run also passed; initial upstream fallthrough warnings are
locally suppressed without restyling shared code. Existing libcdio/wimlib warnings
remain. No XZ device/helper/guest/physical/boot qualification is claimed here.

Evidence: .rufus-test-artifacts/cp25a, snapshots/diff, provider/binary/source hashes,
125202-byte hook-disabled preprocessing equality, five changed plus two added
production files and 714 unchanged hashes, complete normal/sanitizer logs and
cleanup. Test TemporaryDirectory/C fixture cleanup completes; no large artifacts
are retained. Preserve user source ISOs, Mac recovery USB and the old system-managed
CP24c cores. No additional Windows customization installation tests are scheduled.

Next: qualify XZ file writer and strict helper/protocol/controller admission, then
real guest creation/readback before ordinary XZ USB enablement. Continue the other
original compression/container/save/force-marker and advanced/legacy features.
The complete Windows parity objective remains active and unfinished.


## CP25b — XZ writer, strict helper admission and process qualification (2026-10-18)

Verify all 721 CP25a production hashes. Admit XZ to the existing compressed plan,
regular-file/block writer and strict compressed-write-device-v1 protocol. Keep
ordinary SELECT explicitly gzip-only until real guest XZ qualification. The
front end now holds this admission guard as the backend gains XZ support.
Five existing production files change, no production files are added and 716
remain byte-identical. Source/XZ/hash algorithms, the helper entry/native owner,
source/home/claim/cleanup, process state machine and raw controller stay unchanged.
The target-inspection/transfer/sync/readback engine is also byte-identical.

Use the same four-pass helper engine for gzip and XZ: freeze exact decoded size,
source identity, SHA256, prefix/member/trailing facts and sector-rounded bytes;
reconstruct before the first write; decode into 64-KiB sector-aware transfers;
hold the final partial batch until successful stream/hash/source validation;
zero-pad, sync dirty work even after a failure, then independently decode/compare
all physical output and recheck source/target identity. Preserve caller ownership,
o-follow source validation, alias/capacity protection and checked target/home/source
closure. Preserve primary write/cancel versus sync/close error priorities. No new
helper, privilege prompt, production expanded image, eject or writable remount.

The wire layout/version and its five compression fields are unchanged. Accept
original type 6 only with an XZ filename and all existing exact field/type/key,
canonical count, sector, SHA256, capacity/alignment and raw-DD constraints.
Unsupported IDs, mixed suffix/type, fractional/string types, missing/extra keys,
legacy operation with compression fields and stray compressed metadata on raw
requests remain invalid. Gzip and legacy packets keep their old shape and behavior.
Padded decoded bytes continue to drive progress and completion; stored or unpadded
decoded totals cannot stand in for written totals.

Normal and ASan/UBSan/leak builds qualify all ten current affected suites. XZ has
139 regular-file plan/geometry/copy/flush/readback cases and 55 native helper jobs;
gzip regressions pass 232 file cases and 49 helper jobs. Process tests pass 59
raw/gzip and 33 XZ QTest results, and the complete analysis suite passes 56 results
(including framework setup/cleanup). Retain 1029 gzip source checks, 71 gzip corpus
cases, 966/967/970 XZ source checks and 220 XZ corpus/differential/large cases. The
former provider-only backend-admission refusal is replaced by real write admission
and a test proving a valid XZ backend plan/hash while ordinary Qt selection fails,
clears its source and cannot start. No ISO scan or decoder call is routed through
that gated selection. Gzip's ordinary analysis/worker/cancellation behavior passes.

Independent Python liblzma/hashlib and complete target readback cover 512/4096/65536
sector plans, exact decoded/padded bytes, preserved target suffix, stored sources
larger than capacity, first-stream-only tails/padding/concatenation, every supported
check kind, explicit marker force, short/EINTR I/O, concurrent hashing/planning,
forged compression and all previous frozen-plan/source/copy/cancel/sync/readback
faults. Exercise every allocation failure in the writer's actual decode passes.
Native helper jobs use real production dispatch, planning, write/sync/readback and
cleanup over owned regular files, with link-only root/kernel/UDisks/mount/selection
facts and a real advisory exclusive lease. They test pre-claim CRC32/CRC64/SHA256,
size/hash/ownership refusal, dirty errors, changed geometry and checked close.
This is not actual kernel block I/O, physical USB, host polkit or boot proof.

Protocol children assert one authorization launch, valid phases/counts, failure
and cancellation, delayed result/child exit, cleanup errors and terminal state.
Both app/helper builds complete. Initial combined protocol rows pass all 88
results in both builds, but the sanitizer invocation takes 29.85 seconds against
its existing 30-second bound. Separate XZ rows into a second 30-second CTest run,
retaining every assertion and legacy deadline. Final normal raw/gzip and XZ runs
take 14.63/8.00 seconds; sanitizer runs take 15.60/8.26 seconds. Only these affected
process tests need repeating after the split; the eight other passing suites
retain unchanged test/runtime inputs. No production deadline or validation is
weakened. An initial test-only compile error from a renamed boolean is fixed and
its failed build log retained. Existing libcdio/wimlib warnings remain.

Evidence: .rufus-test-artifacts/cp25b, 721-file before/after inventories, five scoped
production changes, 716 unchanged hashes and engine-reuse proof, source snapshots/
diff, actual build/provider/binary hashes, all initial and final test logs, exact
limits and cleanup. Tests remain unprivileged outside the private socket/process
inspection sandbox under explicit user authorization. All owned disposable sources,
targets and sparse large fixtures are removed; no expanded production image or
large fixture is retained. Preserve user sources and Mac recovery USB. Do not
retry the old system-managed core removal or add Windows customization installations.

Next: real guest-kernel XZ helper/claim/flush/readback and bootable-image restore,
then ordinary Qt XZ selection/creation with joined completion. Continue remaining
compression providers, Unix-compress fallback, VHD/VHDX/FFU, force-marker UI, saving
and all advanced/legacy Windows parity requirements. The full goal stays active.


## CP25c — real guest-kernel XZ helper and bootable readback (2026-10-18)

Verify all 721 CP25b production hashes. Extend the guarded MBR guest recipe with
--xz-raw-helper, keeping its 25 seed/raw/gzip jobs and adding 20 XZ jobs. All
production files, Windows/shared/native compression/hash algorithms, helper/job/
protocol/controller and ordinary XZ admission gate remain unchanged. Reuse the
CP24d guest fault probe byte-for-byte; only guest fixtures, reporting and recipe
orchestration change. Normal and ASan/UBSan/leak guests pass all 45 jobs each.

The helper runs as actual guest root on a real 64-MiB USB block disk with observed
512-byte sectors. Source/caller-home identity and ownership, kernel geometry,
mount checks, unmount, exclusive O_EXCL claim, positioned writes, fsync, readback
and checked close remain native. Private UDisks properties and counted/held pkexec
are fixtures. Stage the independently built xz CLI and its dependencies only as
an encoder; production continues using Rufus's original XZ Embedded decoder.
Generate caller-owned XZ streams with an explicit 64-KiB dictionary and one
encoder thread. Export encoded stream/check/hash facts and actual input hashes.
No external encoder or liblzma is added to production dependencies.

XZ's four initial refusals cover frozen decoded hash/size, actual sector mismatch
and a corrupt CRC64 data check, with the complete disk unchanged. Four mounted
jobs exercise busy/protected caller-home refusal, EOF cancellation after native
unmount and complete creation. Six dirty/observed jobs start from poisoned bytes:
17 actual bytes followed by EIO; cancellation after 65536 actual bytes; actual
fsync then EIO; real readback with a test-only byte flip; real close then EIO;
and observed success. O_EXCL contenders must receive EBUSY through write, flush,
readback and pre-close, then reacquire after one checked close. Assert physical
counts, cleanup/started facts, sync failure retention, joined descriptors/children,
one authorization launch and no completion while its launcher is held.

Independently compare every written prefix byte, explicit zero padding (1 MiB
+ 37 -> 1 MiB + 512), and a hash of every byte beyond actual writes. Add CRC32 and
SHA256 data-check refusal/success pairs and a valid no-data-check stream, using
poison before each successful replacement. The final unchanged production helper
restores all 67108864 bytes from CRC64 XZ. Full guest and host SHA256 agree with
the original raw source and prior gzip restore; independent host MBR/FAT/files
checks pass. A second BIOS VM executes Syslinux/menu/boot-sector RUFUS_RAW_BOOT_OK,
with the backing image hash unchanged. This is bootable fixture-image execution,
not a Linux kernel or installed OS. No physical media, host polkit, other actual
sector size, power-loss persistence or ordinary XZ Qt creation is claimed.

Initial normal attempts are retained. The first stops at the unchanged 15-second
legacy mounted raw deadline before gzip/XZ; no timeout-phase diagnostic existed,
and its cause is unproven. Host I/O pressure was observed after the failure. Add
an observation at 14 seconds for legacy mounted jobs without extending their
bounds; the next raw-success takes 3843 ms. That run passes all baseline and small
XZ cases, then the full XZ image reaches the reused 90-second gzip fixture bound.
Host CPU contention is observed, and the original LZMA2 and native CRC64 Debug loops
have substantial TCG cost. Add 15-second phase/byte diagnostics and a separate
300-second bound only for the new complete-XZ-image job. The new 45-job XZ recipe
gets 1200 seconds; existing gzip recipe remains 600, gzip jobs 90, compressed
mounted jobs 60 and legacy mounted jobs 15. All data, failure, claim, integrity,
cleanup, exit and boot assertions remain. No product deadline changes.

The initial sanitizer workspace-backed attempt also reaches the unchanged legacy
raw-success deadline. Its new 14-second diagnostic records completed copying and
RAW_WRITE_SYNC, without a held exit marker. Run the identical binaries/cases and
bounds from a private /tmp tmpfs directory to remove host storage-flush variability;
export every result and remove that temporary parent. The normal run retains
workspace-backed I/O proof. This changes the host backing file, not guest block
I/O, fsync calls, claims, error injection or assertions; no physical persistence
claim is made for either VM. The runner checks a real non-root host UID. A guard
initially rejected duplicate findmnt tmpfs rows before guest creation; normalize
identical rows and run under the existing explicit local-test authorization.

Evidence: .rufus-test-artifacts/cp25c, unchanged 721-file production inventories,
source snapshots/diff/reuse proof, actual build/provider/binary input hashes,
failed and final normal/sanitizer serial/counters/claims, full independent disk
hashes/boot output and cleanup. All generated disks, roots, archives, copied ISO
trees and readback files are removed, including failed attempts. Retain current
builds and the small qualified installer for front-end qualification. Preserve
user sources/Mac recovery USB and the old system-managed CP24c cores. No additional
Windows customization installation matrix is introduced.

Next: admit XZ in asynchronous ordinary Qt selection, freeze accepted source/
decoded identity and target geometry, then qualify confirmed single-helper DD,
physical progress and joined READY through the guest controller. Remaining
compression/container/save/force-marker and advanced/legacy Windows parity work
continues; the complete objective remains active and unfinished.


## CP25d — ordinary XZ selection and confirmed Qt DD creation (2026-10-18)

Verify CP25c's 721 production hashes. Admit XZ beside gzip on the existing
cancellable/joined ImageAnalysisJob worker. Add *.xz to ordinary SELECT and use
format-neutral compressed-image status text. Freeze retained source identity,
actual decoded size, SHA256, compression/report facts and the strict 55 AA marker
through the same complete validation already used by gzip. Keep target-independent
512-byte analysis geometry separate from the selected target's observed sector.
START performs the existing source/target/sequence/geometry checks before the
single confirmation and authorization. It does not decode on the GUI thread.

Only four existing production files change: the admission/picker/status and the
focused test registration in CMake; 717 hashes remain unchanged. Original Windows/
shared algorithms and native source/XZ/hash/writer, protocol, target acquisition,
source/home/claim/cleanup, process and raw controller bodies are unchanged. Reuse
CP25a–c's XZ dictionary/first-stream behavior and stronger CRC64/SHA256 integrity.
The ordinary flow makes five complete decode passes (one SELECT plus the helper's
four), with bounded buffers and no expanded production image. Other providers,
force-marker UI and containers are still pending rather than silently admitted.

Normal and ASan/UBSan/leak host runs pass the complete 64-result analysis suite
and 63 focused Widgets results, including framework setup/cleanup. Parameterize
existing gzip contracts for XZ: accepted decoded identity/report and worker affinity,
picker/status, corrupt actual data check/truncation/marker/short/unsupported refusal,
queued cancellation, replacement across formats and source mutation after decode.
Use independent xz fixtures; random XZ input proves stored size can exceed a
fitting target. Test observed 512/4096/65536 geometry, padding/capacity/unknown-sector
refusal, changed source/sector/sequence before authorization, one launch, dirty
write/sync/readback failure and cancellation. Held success stays at 99 while the
helper runs, then full green READY/100 and one beep/alert after joined exit; device
removal after success preserves that state and does not repeat its notification.
These Widgets targets are protocol fixtures and never open a host block device.

Add rufus_compressed_write_widgets with a bounded 120-second focused run; retain
the existing broad Widgets 600-second bound and do not repeat its unrelated
matrix. Both complete analysis and focused suites pass on their first runtime
attempt. A guest-test assertion initially referenced ddRequired on Selection,
where that field does not exist; retain the existing rawImage/DD route check and
assert the actual source/report fields. Both rebuilt guest targets then pass.
Existing libcdio/wimlib warnings remain; no production warning/gate is suppressed.

Both disposable guest recipes pass 47 jobs: CP25c's 45 plus ordinary gzip and XZ
SELECT -> START -> confirmation -> production helper -> joined completion. Each
controller restore begins with poisoned 4096 prefix bytes and the final 512-byte
sector, then restores all 67108864 bytes. Assert frozen decoded type/size/hash and
report facts, actual 512-byte geometry and physical totals, no authorization during
analysis or before confirmation, one helper and no completion while its launcher
is held. Full Qt/guest/host raw/gzip/XZ hashes agree; independent MBR/FAT/files and
BIOS Syslinux/menu/boot-sector execution pass with unchanged backing media.

The guest front-end models UID1000 only in its main thread; its actual kernel
credentials and the unchanged exec'd root helper remain guest root. UDisks metadata
and counted/held pkexec are fixtures. Normal and sanitizer host tests run as the
real unprivileged user outside private-socket/process-inspection restrictions under
explicit authorization. Both new VM runs use private tmpfs backing files after
CP25c exposed host flush variability, retaining native guest block I/O, claims,
fsync calls, faults and all assertions. No physical media, host polkit, other real
sector size, power-loss persistence or installed-OS/Linux-kernel boot is claimed.

The new XZ Qt fixture permits 180 seconds for its full worker scan, 300 for its
helper and 600 for its guarded child; the new 47-job recipe permits 1800 seconds
for Debug/sanitizer TCG and ten additional large decode/hash passes. Existing gzip
Qt and direct-helper/legacy bounds remain unchanged. The GUI/product adds no new
timeout. The desktop GUI remains unprivileged in ordinary use; no additional
privileged process, eject or writable remount is introduced.

Evidence: .rufus-test-artifacts/cp25d, scoped snapshots/diff, four production
changes and 717 unchanged hashes, actual builds/providers/binary/source/guest
input hashes, passing 64/63 host logs, both 47-job serial/claim/controller/readback/
boot results and cleanup. Export small evidence then remove every owned disk,
root, archive, copied tree and private tmpfs parent. Preserve user sources/Mac
recovery USB and the old system-managed CP24c cores. No extra Windows customization
installation matrix. The ordinary supported compressed formats are now gzip/XZ.

Next: continue the remaining original compression providers (BZIP2/LZMA/Unix
compress/ZIP/Zstd/VTSI), containers, force-marker UI, saving and all advanced/legacy
Windows parity requirements. The complete objective remains active and unfinished.


## CP26a — original BZIP2 decoder and retained-source foundation (2026-10-18)

Verify all 721 CP25d production hashes before work. Extract the original
bit/Huffman/Burrows–Wheeler/RLE/output decoder from decompress_bunzip2.c into
src/bled/bunzip_core.inc without restyling its algorithms. Keep the original
Windows initialization, stream wrapper, help-data wrapper and behavior intact.
Add the include to Automake source distributions and Visual Studio file lists;
both project XML files parse. With the two native hooks disabled, preprocessing
the complete old/new Windows translation unit is byte-identical: 11127 bytes.
This proves extraction/reuse, without claiming a Windows compiler/build run.

The native compressed_bzip2 adapter adds only per-operation I/O state and an
optional refill hook to the shared core. It uses retained ordinary-source pread,
retries EINTR/short reads, preserves caller ownership/offset and distinguishes
unexpected physical EOF from a malformed compressed trailer. Every allocation
is owned by a heap reader outside the setjmp boundary, with no changed automatic
local inspected after longjmp. Cancellation, I/O/consumer errors, output bounds
and allocation failures release all state and clear the public result through
the existing common source API. No mutable global/TLS decoder I/O is introduced.

Keep original block sizes h1..h9 and unsupported obsolete randomized blocks.
The original intermediate uint32 buffer is at most 900000 entries (3600000
bytes); RLE can expand beyond that. Two 64-KiB I/O buffers and fixed decoder/
CRC state are per operation. Tracked provider allocation payload peaks at
3772408 bytes; the contract bounds it by 3600000 plus 256 KiB. Count real output/member/source/tail sizes in
64 bits, retaining only the first 512 decoded bytes. Validate block and combined
CRCs for every contiguous BZ member, join pbzip2-style concatenation, discard
only original member padding bits and report a non-BZ tail as on Windows.
Results publish only after source identity and cancellation are rechecked.
Callbacks remain provisional until complete checks succeed.

An executable differential test uses the unchanged original Windows wrapper
with ordinary host-file shims and the same original core. It proves a concrete
integrity defect: a damaged empty-stream combined CRC logs an error but can
return success with zero/the preceding output count. Native validation returns
EBADMSG for the empty corrupt stream alone or before/between/after valid data.
Accepting such a stream would violate the source API's verified-result contract;
this stricter native behavior is intentional and tested. The original Windows
wrapper remains unchanged. Valid members, output and trailing-data behavior
agree with both the original wrapper and Python's independent BZIP2 decoder.

Normal and ASan/UBSan/leak runs pass all ten focused targets. BZIP2 has 180
corpus/differential/64-bit-source cases: all nine block levels, empty/short/sector/
64-KiB boundaries, incompressible/multi-block/RLE input, concatenated empty/data
members, a BZ header split at byte 65535/65536, trailing data, every byte of each
actual block/combined CRC, a later member after corruption, truncated headers/
data/checks, invalid signatures/levels/original pointer, randomized refusal and
output limits. A sparse file above 4 GiB checks stored-source/tail accounting;
its ignored tail is not claimed to be a decoded member or a read through 4 GiB.

The large corpus independently encodes bounded input chunks, then counts actual
4294967808 decoded bytes across 65 members and separately refuses a 4294967296
byte limit. Both are real complete decoder passes, with no expanded image file;
the packed fixture is below 256 KiB. Two source contract runs pass 654/851 checks
for one/two members, with two/three allocation positions faulted, concurrent
readers, untouched caller offset/FD count, short reads/EINTR/I/O/early EOF,
consumer errors, cancellation and source timestamp/replacement/removal.

Retained regressions pass: gzip source contract and 71 corpus/large-stream cases,
XZ contracts and 220 corpus/differential/large-stream cases, compression policy,
gzip/XZ writer suites, all 64 analysis results and 63 focused Widgets results.
Qt/private-socket tests run as the real unprivileged user under existing explicit
authorization. Normal/sanitizer app and helper builds also pass. Original decoder/
wrapper signedness and existing libcdio/wimlib warnings remain unsuppressed.
New BZIP2 small/large bounds are 180/600 seconds (each large probe 240); no existing
test deadline changes. The earlier 179-case development corpus also passed before
the boundary, two-member fault and large-output checks were added.

Eight existing production files change, three are added and 713 hashes remain
unchanged (724 current files). The XZ/gzip decoder bodies, native compressed
writer, protocol, helper, target/source/home/claim/cleanup, raw controller and
ordinary Qt allowlists are unchanged. Common read-only source dispatch now
admits BZIP2, but write/helper/ordinary SELECT admission still accepts only
qualified gzip/XZ. No BZIP2 media-creation/guest/READY qualification is claimed.
Production adds no external bzip2 executable or libbz2 dependency; independent
Python BZIP2 is a test provider. No extra authorization or host block device.

Evidence: .rufus-test-artifacts/cp26a; scoped snapshots/diff, preprocessor/XML/
distribution proof, before/after/source/binary/provider hashes, both passing
10-target logs, checksum differential, allocation/concurrency/large counts and
cleanup. Temporary corpus/contract/rate/sparse files are removed. Keep current
builds for the next BZIP2 integration checkpoint; preserve user sources, the Mac
USB and the old system-managed CP24c cores. No new Windows customization matrix.

Next: freeze BZIP2 source plans and qualify the same native writer/helper and
real guest/Qt completion route before ordinary admission. LZMA/Unix compress/
ZIP/Zstd/VTSI, containers, force-marker UI, saving and every remaining advanced/
legacy Windows parity requirement stay in scope. The complete goal remains active.


## CP26b — BZIP2 frozen plan, writer and strict native helper (2026-10-18)

Verify all 724 CP26a production hashes. Admit BZIP2 in the two existing native
compressed-plan/writer guards and two protocol guards. Update the API comment
and register three bounded BZIP2 test targets. Only four production files
change; 720 hashes remain unchanged and no production file is added. Normalize
only those guard additions and the entire writer/protocol bodies match CP26a.
Original Windows/shared algorithms, native BZIP2/gzip/XZ readers, source/hash,
target validation, ownership/home/mount/claim/cleanup, helper, process adapter,
raw controller and ordinary image-analysis/picker admission remain unchanged.

Freeze retained source identity, type 5, actual decoded size, SHA256, first-sector
marker, member/tail counts, observed sector and padded target size through the
same COMPRESSED_WRITE_PLAN. Preserve complete preflight, buffered positioned
writes, dirty fsync, complete decoded/padding readback and source/target rechecks.
The helper makes the same four decode passes: plan, revalidation, copy and verify.
No expanded image is stored. Validate exact compressed-write-device-v1 schema:
source has seven fields and compression has type, canonical decoded-size string,
integer sector, lower-case SHA256 and boolean ignoreMarker. Other pending types,
wrong extension/type, malformed numeric/hash/policy fields and extra/missing
fields remain refused. One helper owns the claim through cancellation and checked
closure; a result is accepted only after successful process exit.

Normal and ASan/UBSan/leak runs pass all ten targets on their first runtime
attempt. BZIP2 regular-file qualification passes 138 cases, with actual poisoned
file readback and untouched tail/source assertions: 512/4096/65536 sectors,
sector boundaries/padding, stored-versus-decoded capacity, marker/forced-marker,
empty and tiny concatenated members, non-BZ tail, BZ input-header split at byte
65535/65536, repeated short I/O/EINTR and concurrent complete planning/stored-file
hashing. Each frozen-plan fact is invalidated independently before writing.
Every allocation position in the matching four-member fixture is faulted, with
no leaked allocation or FD and preserved caller source/target offsets.

Keep all existing destructive boundaries: I/O/EOF/cancellation after the first
65536 target bytes, actual 17-byte partial writes, failed final padded writes,
late checksum failure after 196608 bytes, dirty sync and simultaneous errors,
source mutation during copy/sync/verify, changed target size/flags, readback/EOF/
cancel/corruption and corrupt zero padding. Sync occurs exactly once when dirty,
including failed/cancelled work; partial byte counts and independent target bytes
must agree. BZIP2 decodes a whole BWT block before emitting bytes, so use a valid
member at each 64-KiB decoded boundary for post-write source-I/O fault cases.
The allocation baseline uses that identical stream, and copy corruption flips
an actual final combined-CRC byte. This retains real failure boundaries rather
than depending on a gzip/XZ input/output refill relationship. Tiny members use
independent h1 encoding and still aggregate to two target writes/readbacks.

The actual helper/job/protocol engine passes 56 BZIP2 jobs on owned regular files
with link-only selection, UID and kernel facts. Reuse snapshot/mount/home/claim/
block/sector/capacity/readonly and checked source/target/home close faults,
source ownership/identity/hash/size refusal, protected-target unmount ordering,
17-byte dirty failures, sync/readback/padding errors and EOF cancellation. Each
claimed target closes once before source cleanup; errors never become success.
Add actual block/combined-CRC refusal, concatenated/tail success and corrupt empty
combined CRC alone/before/between/after valid data, all refused before a target
snapshot, claim or write. Stored BZIP2 may exceed an exact fitting target. All
three simulated sectors and source/target bytes are checked independently.
These are simulated native facts, not a real kernel block-device qualification.

BZIP2 protocol/process tests pass 33 QTest results, including setup/cleanup,
strict request round trips, physical padded totals versus stored/decoded totals,
one launch, cancellation/destruction, unknown/dirty state, sync/close/exit errors,
invalid transitions/frames/results and held result versus joined child exit.
Retained raw/gzip and XZ protocol runs pass 59/33 results separately under their
unchanged 30-second bounds. Retain gzip/XZ file-writer 232/139 and helper 49/55
cases. Complete analysis passes 65 results: a valid independently encoded BZIP2
file produces the exact native decoded hash/type plan, while ordinary SELECT
still fails, retains no usable source and does not offer *.bz2 in its picker.
All previous 64 gzip/XZ/analysis results remain. Qt encodes this one fixture with
the independent bzip2 CLI; production does not use that executable or libbz2.

Both ordinary-user normal/sanitizer app/helper and fixture builds pass with
Ninja default parallelism. Run private-socket/process tests under existing
explicit authorization, with no host block device or physical media. New BZIP2
writer/helper/protocol test bounds are 90/90/30 seconds; existing bounds and
assertions stay intact. Existing libcdio/wimlib diagnostics remain unsuppressed.
Reuse CP26a's identical source/core hashes and its 180-case/actual >4-GiB source
qualification; do not repeat that unchanged corpus in this backend checkpoint.
No real BZIP2 guest/BIOS or ordinary Qt USB-creation/READY qualification is claimed.

Evidence: .rufus-test-artifacts/cp26b, scoped before/after/diff, production/reuse
proof, actual source/binary/provider/build flags, both passing ten-target logs
and cleanup. Remove disposable source/target/protocol/Qt fixtures; retain current
builds for the next BZIP2 guest/controller checkpoint. Preserve user sources, the
Mac recovery USB and old system-managed CP24c cores. No extra Windows installation
customization matrix. Ordinary supported compressed creation remains gzip/XZ.

Next: qualify real guest BZIP2 block claims, failures, full restore/readback and
bootable backing, then ordinary worker SELECT/confirmed controller/joined READY.
Continue LZMA/Unix compress/ZIP/Zstd/VTSI, containers, force-marker UI, saving and
all remaining advanced/legacy parity. The complete Windows goal remains active.


## CP26c — real BZIP2 guest block/helper qualification (2026-10-18)

Verify and retain all 724 CP26b production hashes. No original/shared algorithm,
native BZIP2/gzip/XZ source, hashing, writer/geometry, protocol, helper, source/
home/mount/claim/cleanup, process/controller or ordinary admission body changes.
The native guest fault probe is also byte-identical. Extend the guarded MBR
recipe/driver with BZIP2 independently encoded by the bzip2 CLI and staged with
its actual libraries; the production decoder does not use this external provider.

The new --bzip2-raw-helper implies the complete old --xz-qt-helper recipe.
Normal and ASan/UBSan/leak runs each pass 69 jobs: all 47 seed/raw/gzip/XZ helper/
ordinary-controller regressions and 22 new BZIP2 helper jobs. BZIP2 adds 18 direct
cases and four mounted-target cases. Validate metadata digest/size/observed-sector
refusal and actual first-block/combined CRCs, including corrupt empty streams
alone/before/between/after valid members. Check every refused job leaves the
entire 64-MiB device unchanged. Verify concatenated empty/data/empty streams and
non-BZ tail success, with independently compared decoded prefix and zero padding.

The guest runs the actual root helper against a real Linux virtual USB block
device with native sysfs/ioctl/source ownership, private-home/mount checks and
O_EXCL block claims. Busy target directory and caller-home-on-target are refused;
EOF cancellation after real unmount reports no writes; complete mounted writing
succeeds. Six native fault/observation jobs perform actual 17-byte partial writes,
65536-byte cancellation, real fsync followed by EIO, a real readback buffer fault,
real close followed by EIO and observed success. The unchanged fault probe proves
the exclusive claim remains held during writes, sync, readback and checked close,
closes once, and is reacquirable after joined completion. Preserve exact dirty
sync and partial-write/verified counts, bytes beyond actual writes, source data,
FD count and child-process cleanup. All three formats reuse the same assertions.

Before the full BZIP2 restore, poison all 67108864 target bytes with distinct
bytes and sync/close that test claim. Restore every byte through the unchanged
four-pass native helper, compare the full independent guest disk hash, then
require host raw/gzip/XZ/BZIP2 hashes to agree. Independently inspect MBR/FAT32,
filesystem records and file contents; BIOS Syslinux/menu/boot-sector execution
must emit RUFUS_RAW_BOOT_OK and preserve backing-media SHA256. The test's owned
raw source and poison buffer are fixtures, not an expanded production image.

Retain both ordinary gzip/XZ SELECT -> START -> confirmation -> one helper ->
joined completion jobs, each restoring all 64 MiB from a separately poisoned
prefix/end sector. Their frozen source/hash/type/report, native geometry, absence
of early authorization, held launcher and final counters/hashes remain asserted.
BZIP2 ordinary SELECT stays gated: this checkpoint qualifies the backend on a
real guest block device, without claiming its ordinary controller/READY path.

The first normal run stopped in the retained ordinary gzip helper-completion
90-second bound after direct gzip full restore had passed; it did not reach
BZIP2. Its cause remains unproven. Preserve that terminal failure and all small
logs/manifests; remove its owned large files/private parent. Add only 15-second
Qt progress diagnostics for type/state/phase/written/verified/held/status before
retrying. Keep every existing deadline and assertion unchanged. Successful runs
retain those diagnostics. New BZIP2 independent encoding/full-helper bounds are
120/300 seconds, and the new complete 69-job recipe permits 2400 seconds for
Debug/sanitizer TCG. No GUI/product timeout changes or warning suppression.

Both hosts run as UID1000 with QEMU TCG, no network/host device passthrough, and
private /tmp tmpfs backing after CP25c exposed workspace flush variability.
Actual guest logical sectors are 512 bytes. UDisks metadata and counted/held
pkexec are fixtures; the legacy Qt front-end models caller UID1000 only in its
main thread while kernel credentials/helper remain guest root. Do not claim
host desktop-polkit, physical media, other actual sectors, power-loss persistence
or installed-OS/Linux-kernel boot. Reuse CP26a/b source/backend host qualifications
through identical production hashes rather than rerunning unchanged matrices.
No extra Windows customization installation testing is scheduled.

Evidence: .rufus-test-artifacts/cp26c, actual source/binary/provider/build/archive
input hashes, failure diagnostics, both passing 69-job serial/claim/mount/controller/
full-readback/boot results, scoped before/after/diff and all 724 production hashes
unchanged. Remove all owned roots, archives, images, copied trees and private
tmpfs parents after exporting small evidence. Retain current builds/qualified
installer for the next controller checkpoint. Preserve user sources, Mac recovery
USB and old system-managed CP24c cores.

Next: admit ordinary BZIP2 worker selection and qualify frozen plan/confirmation,
physical padded progress, one helper, failures/cancel, held completion and joined
full green READY/one notification. Keep LZMA/Unix compress/ZIP/Zstd/VTSI, containers,
force-marker UI, saving and all advanced/legacy Windows parity in scope. A saved
read-only LZMA audit is preparatory, not an implementation. The full goal remains
active and unfinished.


## CP26d — ordinary BZIP2 SELECT and confirmed Qt creation (2026-10-18)

Verify all 724 CP26c production hashes. Admit BZIP2 beside gzip/XZ on the existing
complete ImageAnalysisJob worker and add *.bz2 to the ordinary SELECT picker.
Only two production files change; 722 hashes remain unchanged. Normalize the
provider guard and filter and both entire files match their previous bodies.
Original Windows/shared algorithms, source/BZIP2/gzip/XZ readers, hashing, native
writer/geometry, protocol, helper, target/source/home/claim/cleanup, process and
raw controller bodies remain unchanged. Existing format-neutral status text is
reused. No new runtime provider, privileged process or authorization is added.

The worker validates the complete retained stream, actual decoded size, SHA256,
strict first-sector marker and source identity before publishing accepted DD
metadata/report facts. Analysis remains cancellable and joined, with canonical
512-byte analysis geometry. START freezes observed target sector/capacity/disk
sequence and revalidates source/geometry before confirmation and one helper.
Creation makes five complete decode passes: one worker scan and the helper's
four existing passes. Bounded native buffers/state are reused; no expanded
production image. BZIP2 retains original concatenation/tail behavior and CP26a's
intentional refusal of corrupt empty combined CRCs accepted by Windows.

Normal and ASan/UBSan/leak runs pass all 80 complete analysis and 81 focused
Widgets results, including framework setup/cleanup. Add accepted BZIP2 identity/
report/worker-affinity/picker/status and CRC/truncation/marker/short refusal rows.
Move the pending-provider refusal suffix to .lzma. Check all six directed pairs
of different accepted providers during queued replacement, cancellation in all
three formats and source changes after decoding. Four independently encoded
corrupt empty BZIP2 placements (alone/first/last/middle) must fail with no usable
source or START. Reuse independent bzip2/xz fixture encoders only in tests.

All three formats test actual plan sector observations at 512/4096/65536,
decoded/padded capacity and unknown geometry, changed source/sector/sequence
before authorization, one launch and write/sync/readback failures/cancel. Random
BZIP2 fixture bytes prove the stored source can exceed an exact fitting target.
Hold a successful helper result while its process remains live: the GUI stays
at 99 with no completion beep/alert, then reaches full green READY/100 and one
beep/alert after joined exit. Unplugging after success retains completed READY
without another notification. These host targets are private protocol/kernel
fixtures and never open a host block device. Reuse the focused CMake target;
do not repeat the unrelated broad Widgets matrix or change its deadline.

A new --bzip2-qt-helper recipe qualifies one complete 67108864-byte BZIP2 ordinary
controller job with five necessary bootable seed jobs and five raw setup jobs
(11 total). Reuse CP26c's normal/sanitizer 69-job native backend qualification:
all backend/source production hashes and both production helper binaries are
byte-identical. New production changes only admission and picker, so exercise
the complete new UI route without repeating unchanged helper fault matrices.
The original --bzip2-raw-helper and all older modes/checks remain available.

Extract the existing per-Qt target poison, identity pipe, child-start/join and
exit assertions into one named guest operation. Keep the original gzip/XZ
behavior/bounds and use it for the new BZIP2 job. Poison the first 4096 bytes and
final 512-byte sector before the ordinary restore. The guest asserts worker
frozen type/stored/decoded sizes/SHA/report, native 512 geometry and physical
totals before confirmation, no authorization during analysis or before consent,
one production helper, no completion while its launcher is held and exact
joined written/verified counts. Require complete controller/guest/host BZIP2
hash equality, independent MBR/FAT/files and BIOS Syslinux/menu/boot-sector
execution with unchanged backing-media SHA256. Both normal/sanitizer 11-job
recipes pass on their first runtime attempt; retain the progress diagnostics.

New BZIP2 worker/helper/guarded-child bounds are 180/300/600 seconds and the
focused 11-job recipe permits 900. Old gzip/XZ/legacy bounds and assertions are
unchanged; the GUI/product introduces no timeout. Both current app/helper/guest
and host test builds pass using Ninja default parallelism. All host tests/VMs
run as ordinary UID1000 under existing authorization, using private sockets and
private tmpfs backing, no network or host device passthrough. Guest kernel/helper
credentials remain real root; only the front-end main thread models UID1000.
UDisks metadata and counted/held pkexec are fixtures. Actual guest sectors are
512; physical media, desktop polkit, other actual sectors, power-loss persistence
and OS/kernel boot from the created media remain unqualified. Existing libcdio/
wimlib diagnostics are unsuppressed. No additional Windows customization matrix.

Evidence: .rufus-test-artifacts/cp26d; scoped before/after/diff, two production
changes/722 unchanged hashes, unchanged helper binaries and CP26c qualification
references, current source/binary/provider/build/archive input hashes, passing
80/81 host and 11-job guest logs/readback/BIOS results, fixture cleanup. Export
small evidence, then remove all owned images, roots, archives, copied trees and
private tmpfs parents. Preserve user sources, Mac USB and old CP24c system cores;
retain current builds for remaining parity work. Ordinary enabled compressed
selection/USB creation is now gzip/XZ/BZIP2.

Next: original LZMA, Unix-compress fallback/provider, ZIP/Zstd/VTSI, containers,
force-marker UI, saving, remaining SELECT/drag/drop and every advanced/legacy
Windows requirement remain in scope. Use the saved primary-source LZMA audit
for the next provider foundation. The full Windows parity goal remains active.


## CP27a — original LZMA-Alone retained-source foundation (2026-10-18)

Verify all 724 CP26d production hashes before starting. Extract the original
range coder, probability models and dictionary decoder from decompress_unlzma.c
into unlzma_core.inc. The Windows wrapper includes that core; default and FAST=1
hook-disabled preprocessor output match their originals byte for byte. Add the
include to both original project/source lists and validate project XML. This is
an extraction/preprocessing proof and host execution of the original wrapper,
not a Windows compiler/build qualification. Eight existing production files
change, three are added, 716 hashes remain unchanged and 727 are current.

Add an internal Linux LZMA-Alone reader to ReadCompressedImageSource. Each call
owns the retained pread input, dictionary, probability table, range coder and
error/cancellation state. No mutable global/TLS decoder I/O or expanded image.
The native owner holds all three decoder allocations across the C setjmp
boundary; all are freed on ordinary return or any nonlocal failure.
Caller FD ownership and position are preserved. Batch short reads/EINTR in at
most 64-KiB requests, distinguish premature retained-file EOF from format EOF,
poll cancellation during buffered decoding and split dictionary flushes into
ordered consumer chunks of at most 64 KiB without a second output buffer.

Preserve all 225 lc/lp/pb combinations and the original dictionary range: zero
becomes one, the cap is 1 GiB, and actual dictionary allocation is at least
4096 and otherwise min(declared size, dictionary). Do not substitute XZ's
64-MiB limit or liblzma's narrower encoder property range. Maximum probability
storage is 6295148 bytes plus bounded range/input/owner state. Current ordinary
4096-dictionary source contracts peak at 85940 bytes. Native global position is
explicitly 64 bits, while disabled-hook Windows declarations remain identical.
Count actual output rather than trusting the 64-bit header size. Check the
caller limit during decoding as well as delivery, so a large dictionary does
not defer a small output limit until its entire buffer fills.

The original stops after the known output size or unknown-size EOS, without
concatenation. Preserve that first-stream behavior and report exact unread
bytes, including unused range-coder flush bytes. A one-byte-refill original
wrapper probe independently establishes consumed offsets without read-ahead.
Known-size streams may finish before EOS and accept their decoded prefix;
do not silently impose a new EOS requirement. There is no LZMA-Alone container
checksum. Reuse source identity and later decoded SHA256 rather than claim CRC
validation or arbitrary bit-corruption detection.

Prove three malformed-input defects in the original wrapper. Its documented
inner match-loop FIXME can emit 4369 bytes for a 4097-byte declaration (and
8446 for 8192 on the pattern fixture); it accepts premature EOS with 20000
bytes against declarations of 20001 or 4294967808; an initial short repetition
can copy unwritten dictionary history. Linux rejects each with EBADMSG and no
published result. Enforce exact known-size output and valid backward history
without altering the original Windows algorithm. These explicit native
differences prevent unusable/mis-sized images or undefined history from becoming
accepted source facts. Bytes already delivered to a consumer remain provisional.

Normal and ASan/UBSan/leak final runs each pass all 12 selected targets. LZMA's
360 corpus/differential/property/64-bit-source cases cover independent Python
liblzma encodings, empty/sector/input/dictionary boundaries, repeated and random
data, known/unknown sizes, original exact hash/consumption, all 225 property
combinations, zero/arbitrary/1-GiB dictionary headers, first-stream tails and
concatenation, truncated consumed data, invalid properties/dictionary, proven
size/history failures, explicit bounds and sparse stored-source/tail >4 GiB.
The independent encoder cannot produce lc+lp>4 directly; single-literal/EOS
fixtures preserve each pb encoding and vary initial lc/lp contexts, then compare
actual original/native hashes and offsets across the full original range.

The retained-source contract passes 30892 checks, exercising all four allocation
failure positions, descriptor/offset preservation, short/interrupted reads,
early and late I/O failures after provisional output, cancellation during input,
callback and buffered decoding, callback errors, source time/path replacement/
removal and concurrent readers. A separate bounded encoder creates a single
small packed stream whose actual output is 4294967808 bytes; count every byte
without an expanded file. A second real decode with a known-size >4-GiB header
refuses the separate 4-GiB output bound. Both builds pass; these are actual
decodes, distinct from the sparse stored-source/tail count test.

Regressions retain gzip 71, XZ 220 and BZIP2 180 vectors, their source contracts,
232/139/138 gzip/XZ/BZIP2 writer cases, all 80 complete analysis and 81 focused
Widgets results. Rebuild both complete app/helpers. Existing provider algorithms,
writer, protocol, helper/claim/cleanup, Qt worker/picker/controller admission and
other production bodies retain their hashes. Ordinary creation remains gzip/
XZ/BZIP2; LZMA writer/helper and ordinary GUI admission remain pending. Do not
repeat unchanged BZIP2 large-output/real-guest failure matrices or unrelated
broad Widgets/customization suites for this source-only checkpoint.

The first new small test assumed unknown-size EOS consumes all final flush
bytes; it failed on a valid unread byte. Establish exact original one-byte input
consumption and correct the test expectation. Production tail behavior was
preserved, no existing deadline was changed, and no runtime failure was hidden.
New corpus/large recipe bounds are 180/660 seconds, with 300 per large probe.
Original signedness and existing libcdio/wimlib diagnostics remain unsuppressed.
Tests are unprivileged private-file/socket fixtures; no physical device, desktop
polkit, actual LZMA USB creation/firmware boot or power-loss persistence claim.

Evidence: .rufus-test-artifacts/cp27a, with scoped originals/current sources/diff,
all production/source/binary/provider hashes, original size/native audits,
default/FAST preprocessing equality, exact differential/contract/large logs,
initial test correction, both final suites and cleanup. Remove owned small
packed, sparse and contract fixtures after checks; retain build caches for the
next provider stages. User source images, recovery USB and old system cores are
untouched. The full Windows parity goal remains active.

Next: LZMA frozen plan/native writer and strict single-helper qualification,
then actual kernel/controller qualification and ordinary Qt admission. Keep
Unix-compress/.gz fallback, ZIP/Zstd/VTSI, containers, saving, force-marker UI,
remaining SELECT/drag/drop and all advanced/legacy requirements in scope.


## CP27b — LZMA frozen plans, native writer and strict helper (2026-10-18)

Reverify all 727 CP27a production hashes and scoped sources before starting.
Admit LZMA type 4 through the existing two writer and two strict protocol
provider guards. Four production files change (writer C/header, protocol and
CMake test registrations), none are added and 723 hashes remain unchanged.
Remove the new clauses and both complete writer/protocol bodies match their
baselines. The original Windows/shared code, native LZMA/source/gzip/XZ/BZIP2
providers, hashing, target I/O/geometry, helper, source/home/target/mount claims,
cleanup, process, ordinary worker/picker and controllers remain unchanged.

Use the existing frozen plan and compressed-write-device-v1 packet. Freeze
source identity, exact type/stored/actual decoded sizes, SHA256, first sector,
member/tail counts, sector and padded total. Keep four complete helper decodes:
source planning, pre-write revalidation, copy and full padded readback. Native
positioned I/O, bounded aggregation, zero padding, dirty sync, primary/separate
sync errors, checked close, protected-home/mount checks and one authorized job
through cleanup/cancellation remain the existing engine. LZMA retains the full
original 1-GiB dictionary/property range, exact first-stream consumption and
CP27a's explicit native size/history refusals. No new runtime decoder/library,
expanded production image, authorization or fallback route is introduced.

Both normal and ASan/UBSan/leak final runs pass all 13 selected targets. LZMA's
regular-file writer passes 145 cases, the simulated-native production helper
passes 60 jobs, strict request/process tests pass 33 results, and complete
analysis passes 81 results (framework setup/cleanup included). Retain gzip/
XZ/BZIP2 writer 232/139/138, helper 49/55/56 and raw/gzip/XZ/BZIP2 protocol
59/33/33 results. CP27a's unchanged source/core qualification is reused with
explicit checks/evidence hashes: 360 LZMA vectors, all 225 properties, 30892
source checks and actual >4-GiB output plus known-size 4-GiB limit refusal.
Do not repeat unchanged large-source or unrelated broad Widgets/customization
matrices for these four admission changes.

Independently encode LZMA-Alone using Python liblzma and use xz --format=lzma
only for the Qt backend fixture. Cover 512/4096/65536 sectors, exact/padded
capacity, target suffix preservation, strict/explicit-force marker behavior,
short I/O/EINTR, concurrent source/hash initialization and all frozen fields.
Unknown and known-size sources, 4096/65536/1-MiB dictionaries and a known-size
1-GiB dictionary header retain the original allocation/range contract. The
original may finish known-size output without a complete EOS; truncate only
the reported unread tail in writer tests and independently prove the helper's
known-size fixture lacks a complete EOS yet restores every declared byte.
Second LZMA streams, XZ streams and junk remain unread first-stream tails.

LZMA has no container CRC. Replace checksum assumptions with actual malformed
size/property/history/truncated-EOS cases. Preflight failures write nothing and
do not acquire a target claim. A substituted size promise of actual+1 must fail
on early EOS, including after 196608 committed copy bytes. Real source-read
failure/EOF/cancel after a 65536-byte commit, 17-byte partial write/cancel,
final-sector faults, sync/readback corruption, identity changes and every
allocation failure retain exact error/phase/counters and independently checked
disk contents. Allocation tracking enforces the original per-operation bounds
and zero live allocations after all success/failure paths. Move the reserved
plan-refusal type from now-supported LZMA to still-unsupported VTSI.

Add a valid alternate stream with identical stored/decoded sizes, type, first
sector, marker, member and exact tail facts but different decoded bytes outside
the prefix. Before injecting it, the C fixture prepares a separate plan for that
stream and asserts every listed fact matches while SHA256 differs. A preflight
substitution returns ESTALE before any target write. Copy substitution returns
ESTALE after 196608 actually committed altered bytes and a dirty sync, with the
suffix untouched. Verify substitution returns EBADMSG after the first unchanged
65536 bytes compare successfully. Original sources and replacement bytes remain
unchanged. These checks qualify the frozen hash/readback defenses rather than
attribute nonexistent integrity checks to LZMA.

The native-helper fixture runs the real helper/job/wire/stream engine while
link-only selection/kernel/UID facts describe owned regular files. Keep source
ownership, device/capacity/sector/sequence, readonly and mount/home checks,
exclusive lease through every source/target I/O and checked close, ordered
source/home cleanup, dirty sync and single result/exit validation. Deliberate
close failures/EINTR remain failures after complete writes/readback; the lease
cannot be released early. Native sector observations are simulated here, not
actual block-device/kernel qualification. The protocol fixture redirects the
one pkexec request to an unprivileged child, validates all four provider types,
cross-type/path mismatch, string/fractional/reserved types, exact fields and
physical padded progress totals, and holds success until child exit. Error/
cancel/close/sync/invalid-frame/exit paths retain the same process contracts.

A Qt test prepares a valid LZMA backend plan/hash, then proves ordinary selection
still refuses it and the SELECT filter omits *.lzma. Complete analysis retains
all three enabled formats, cross-format replacements, corrupt empty BZIP2 and
cancellation/source-change behavior. LZMA ordinary worker/controller/READY is
pending actual guest and GUI qualification; enabled creation stays gzip/XZ/BZIP2.

Both app/helper and fixture builds pass with Ninja default parallelism. First
four-target normal runtime passes writer145/helper59/protocol33/analysis81;
add the same-format second-stream helper case and explicit substitution evidence
line before the final 13-target runs (helper60). There are no build/runtime
failures or deadline changes. New LZMA writer/helper/protocol bounds are 90/90/30
seconds; existing bounds remain unchanged. Sanitizer flags/runtime libraries
and independent encoder hashes are recorded. Host tests are ordinary UID1000
with private sockets/files and soft core limit zero. No host block device,
physical media, real LZMA guest/kernel/firmware, desktop polkit, installed OS or
power-loss persistence claim is made by these fixtures.

Evidence: .rufus-test-artifacts/cp27b, scoped originals/current sources/diff,
all production/source/binary/provider hashes, normalized complete-body proof,
CP27a qualification references, both 13-target logs and fixture cleanup. All
owned sources, replacements, target files and private test directories are
removed. User source images, recovery USB and old system cores remain untouched.
Retain current build caches for the next actual guest/helper stage.

Next: actual guest LZMA source/helper/claim/fault/full poisoned restore/readback/
BIOS qualification, then ordinary Qt worker/picker/controller and joined READY.
Keep Unix-compress/.gz fallback, ZIP/Zstd/VTSI, containers, saving, force-marker
UI, remaining SELECT/drag/drop and all advanced/legacy requirements in scope.
The complete Windows parity goal remains active.


## CP27c — real LZMA guest helper, claims, faults and complete restore (2026-10-18)

Reverify all 727 CP27b production hashes and scoped sources. Extend guarded guest
fixtures and the VM recipe only; every production hash remains unchanged. Both
production helper binaries are byte-identical to CP27b's 13-target qualification.
Original Windows/shared code, native LZMA/source/hash/writer/geometry, protocol,
helper, source/home/mount/claim/cleanup, process and ordinary Qt admission remain
unchanged. Existing gzip/XZ/BZIP2 guest modes remain available with their prior
bounds. Use a focused new-provider mode rather than repeat their historical
69-job matrices; retain the five seed and five raw setup jobs needed by LZMA.

New --lzma-raw-helper runs 38 jobs: five bootable seed/helper jobs, five mounted/
unmounted raw setup jobs, four LZMA mounted protection/cancel/success jobs and
24 direct LZMA cases. Stage independent xz --format=lzma with a 64-KiB dictionary
and no container checksum, plus the real production helper and guarded fault
probe. Source/home ownership is actually UID1000; guest kernel and helper use
real root credentials. UDisks properties and a counted/held pkexec launcher are
fixtures on a private real bus. This stage uses the direct root guest client,
not an unprivileged GUI or ordinary LZMA SELECT/controller qualification.

Native Linux sysfs/ioctl device/capacity/512-byte sector observations, mounted
FAT source/home protection, whole-disk O_EXCL claims, positioned reads/writes,
fsync and checked close remain real. Check competing exclusive opens while the
claim is held, and reacquire it after every joined result. Refusal preserves the
entire disk; every write preserves its unwritten suffix. Independently compare
decoded prefixes, explicit zero padding, exact physical counts and changed disk
contents. Require one launcher/result/exit, retained sync errors, no completion
while the launcher is held, no leaked descriptors or descendants, and source/
home/private-mount release before the driver continues.

All six original guest fault gates run for LZMA: a real 17-byte partial write,
cancellation after a 65536-byte commit, full dirty sync failure, real readback
buffer corruption, checked target-close failure after complete verification,
and observed success. Extend only the test probe to retain/count the real
source FD and check the actual target claim during its reads. Two new modes
return EIO or premature EOF on a source refill after writes begin. Each fails
after exactly 65536 committed bytes with one dirty sync and no readback; the
kernel target claim stays held through source I/O, sync and checked close.
Eight observation cases report actual reads/writes/sync/claim/close counts;
all show one target close and successful later reacquisition. Kernel identity, geometry
and native protection checks remain real. The probe supplies the stated fault
returns, partial write, cancellation hold and readback flip.

Use independently encoded incompressible 1048613-byte small data so the late
source failures require real refills after a committed batch. Compare all bytes
written and every byte beyond the physical boundary. Metadata/hash/size/sector
and malformed size-promise refusals precede target writes. Additional stream
jobs cover known sizes, accepted known-size completion without complete EOS,
ignored second LZMA streams and junk tails, invalid properties/dictionary/
unwritten history, premature declared output above 4 GiB, the proven known-size
match overshoot, truncated unknown-size EOS and a valid known-size 1-GiB
dictionary header. The latter retains the original min(known size, dictionary)
allocation behavior. Independent xz rejects the incomplete-EOS fixture after
producing its exact declared output; the native original known-size behavior
still restores it. No CRC or arbitrary bit-corruption detection is attributed
to LZMA-Alone.

The 24 direct jobs comprise ten no-write metadata/format refusals, six small
successes, seven dirty failures and one complete bootable restore. The four
mounted LZMA jobs cover busy refusal, target-backed caller home refusal,
cancellation after unmount before writes and successful single-helper creation.
They use actual mounts and native mount/home checks, with fixture UDisks
metadata. All remaining jobs run unmounted after the small DD image replaces
the partition table.

Before the final restore, poison all 67108864 target bytes with 0xA5, sync and
close that seed descriptor. Independently compress the original raw bootable
source and restore all bytes through the unchanged production helper's four
decode/hash passes. Require the original raw digest, frozen LZMA digest, actual
guest whole-disk digest and independent host digest to agree. Host inspection
checks MBR/FAT/files and independently boots the restored Syslinux/menu/boot-
sector marker; backing SHA256 remains unchanged after boot. This is bootable
image/firmware execution, not installed-OS/kernel boot from the created media.

Both normal and ASan/UBSan/leak builds and first complete 38-job runs pass.
Normal and sanitizer hashes are recorded separately: generated filesystem IDs/
times can differ between runs, but every within-run source/restore/readback hash
must agree. New LZMA bounds are 120 seconds for encoding, 90 for small helper
jobs, 600 for the four-pass full-image helper and 1500 for the focused guest.
Only the new mode uses these bounds; older mode deadlines/assertions are
unchanged. Keep 15-second helper progress diagnostics. No production timeout,
new authorization, external runtime decoder or expanded production image.

Host runs are ordinary UID1000 with private tmpfs backing, TCG, no network and
no host device passthrough. Guest root credentials, kernel block device and
logical sectors are real; desktop polkit, physical media, other actual sector
sizes and power-loss persistence remain unqualified. Preserve all CP27a source
and CP27b writer/protocol qualification by unchanged hashes. No further Windows
customization installation matrix or unrelated host/broad Widgets suites.

Evidence: .rufus-test-artifacts/cp27c, scoped before/after/diff, all 727 unchanged
production hashes, unchanged production helper binaries, current driver/probe/
helper/encoder/kernel/archive inputs and flags/libraries, 38-job logs/results,
eight claim observations per run, complete readback/files/boot and cleanup.
Export small evidence and remove all owned disk/volume images, roots, archives,
copied ISO trees and private tmpfs parents. Preserve user source images, recovery
USB and old host system cores. Retain current builds for ordinary LZMA Qt work.

Next: admit LZMA on the ordinary complete worker/picker/controller route, with
all four formats' source/geometry/confirmation/cancel/failure/queued replacement
contracts and a focused full-size GUI/helper/held exit/joined green READY guest.
Unix-compress/.gz fallback, ZIP/Zstd/VTSI, containers, saving, force-marker UI,
remaining SELECT/drag/drop and all advanced/legacy requirements remain in scope.
The full Windows parity goal remains active.


## CP27d — ordinary LZMA SELECT, confirmed creation and joined READY (2026-10-18)

Reverify all 727 CP27c production hashes and scoped sources. Admit LZMA beside
gzip/XZ/BZIP2 on the existing complete ImageAnalysisJob worker and add *.lzma to
the ordinary SELECT filter. Only two production files change; 725 hashes remain
unchanged. Normalize the new provider clause/filter and both entire files match
their baselines. Preserve original Windows/shared code, native source/LZMA/
gzip/XZ/BZIP2 readers, hashing, writer/geometry, strict protocol, helper, source/
home/target/mount claims, cleanup, process and raw controller bodies. Existing
format-neutral status/report/confirmation text is reused.

The full worker validates retained source identity, actual decoded size, SHA256
and strict first-sector marker before publishing DD facts. Keep the original
LZMA property range, 1-GiB dictionary cap, known-size completion without complete
EOS, first-stream-only behavior and exact unread tail. Native size/history
refusals remain CP27a. No LZMA container CRC or arbitrary bit-corruption check
is claimed. START freezes observed sector/capacity/disk sequence and revalidates
source/geometry before confirmation and the single helper. Complete ordinary
creation makes five decodes: the worker scan plus the helper's existing four
plan/revalidate/copy/readback passes, without an expanded production image.

Both normal and ASan/UBSan/leak host runs pass all 96 complete analysis and 99
focused Widgets results, including framework setup/cleanup. Extend accepted
worker identity/hash/report/affinity/picker/status and source-change contracts
to all four formats. Remove the prior valid-backend-but-ordinary-gated LZMA test
now covered by real accepted selection. All twelve directed pairs of different
formats test queued replacement during cancellation, plus cancellation for each
provider. Original gzip/XZ/BZIP2 malformed and corrupt-empty-BZIP2 cases remain.
LZMA's malformed rows use known-size promise mismatch, invalid properties,
unwritten history and consumed EOS truncation; removing just a final range flush
byte is not a valid truncation assumption. Move pending-format refusal to .Z.

All four formats cover 512/4096/65536 target geometry, exact/decoded/padded
capacity and unknown sector, stored bytes larger than an exact fitting target,
source changes before START or confirmation, and changed sector/disk sequence
before authorization. Require no target writes or helper launches on refusal.
Successful helper results remain held at 99 with no alert while the process is
live, then reach full green READY/100 and one beep/alert only after joined exit.
Unplugging after success retains completion without repeating notification.
Write/sync/verify errors and copy cancellation remain failed/cancelled with one
launch and no completion alert. Host devices and helper I/O are private fixtures;
the tests assert zero actual host block writes. Keep the existing focused CMake
target and its original bound; do not rerun the unrelated broad Widgets matrix.

New --lzma-qt-helper qualifies one complete 67108864-byte ordinary worker/START/
confirmation/controller/production-helper job with five necessary bootable seed
and five raw setup jobs (11 total). It does not imply older compression/helper
matrices. Reuse CP27c's normal/sanitizer 38-job actual LZMA kernel/source/claim/
failure/protection/readback/boot qualification: both production helper binaries
and all backend production hashes are byte-identical. Retain every old recipe.

Share the existing focused compressed Qt restore operation between BZIP2 and
LZMA; preserve BZIP2's path, source type, poison, packet/child/join checks and
bounds. Before the new GUI job, poison the first 4096 bytes and final 512-byte
sector, sync and close. Feed the independent expected source/target/hash through
the guarded child pipe. Require actual canonical worker/report/type/stored/
decoded/hash facts, native 512-byte target geometry and physical padded total
before confirmation, no authorization during scanning or before confirmation,
one production helper, held exit and exact joined written/verified counters.
The controller, guest and independent host whole-disk hashes agree; independent
MBR/FAT/files and BIOS Syslinux/menu/boot-sector execution pass with unchanged
backing SHA256 after boot. The preceding CP27c stage separately poisoned the
entire target for its native restore qualification.

New LZMA worker/helper/guarded-child/whole-focused-recipe bounds are 180/600/960/
1200 seconds. Keep all old provider deadlines and 15-second diagnostics; no
product timeout is introduced. Both current app/helper/guest and host test
builds pass with Ninja default parallelism. Both first complete host/guest runs
pass. Root guest kernel/helper operations are real. Only the front-end main
thread models caller UID1000; UDisks metadata and counted/held pkexec remain
fixtures. Host runs are ordinary UID1000, with private sockets/tmpfs backing,
TCG, no network and no host device passthrough. Actual guest sectors are 512;
physical media, desktop polkit, other actual sectors, power-loss persistence and
installed OS/kernel boot from the created media remain unqualified.

Evidence: .rufus-test-artifacts/cp27d, scoped originals/current sources/diff,
two production changes/725 unchanged hashes, identical production helpers and
CP27c references, current binary/provider/kernel/archive hashes and flags,
96/99 host and 11-job guest results, independent readback/files/BIOS and cleanup.
Remove all owned targets, roots, archives, copied trees and private parents
after exporting small evidence. Preserve user sources, recovery USB and old
system cores. Retain current build caches for remaining parity work. Ordinary
compressed selection/creation is now gzip/XZ/BZIP2/LZMA-Alone.

Next: original Unix-compress/LZW source and .gz fallback, ZIP/Zstd/VTSI,
containers, force-marker UI, saving, remaining SELECT/drag/drop and every
advanced/legacy Windows requirement remain in scope. A saved read-only audit
of the original Unix-compress core/fallback identifies the next source work;
no LZW implementation or admission is claimed yet. Full Windows parity stays
active until its complete requirements are implemented and verified.


## CP28a — original Unix-compress/LZW retained-source reader (2026-10-18)

The read-only source foundation now accepts `.Z` (type 2). Extract the original
`decompress_uncompress.c` dictionary/bitstream body into `uncompress_core.inc`,
shared by Windows and the native adapter. Disabled-hook preprocessing is
identical to the captured original, including default allocations, width/CLEAR
alignment and original flags. Add the include to both Windows project inventories
and Automake distributions. The new native provider has no executable or library
runtime dependency. Existing gzip/XZ/BZIP2/LZMA bodies, writer, strict protocol,
helper, worker/controller and picker remain unchanged: eight production files
change, three are added and 719 prior production files are byte-identical.

The per-operation reader owns four zeroed buffers/tables plus its owner across
the C jump boundary. Retained `pread` batches short reads/EINTR, preserves the
caller's descriptor/offset and treats EOF before captured size as EIO. Output is
ordered in at most 64-KiB chunks, bounded before consumption, counted in actual
64-bit bytes, and published only after retained source identity and cancellation
checks. All allocation failures, consumer errors, cancellation and malformed
input release ownership and clear the result. The contract fixture measures five
allocations and a 526688-byte peak, including the owner. There is no expanded
intermediate image and no mutable global or thread-local decoder I/O.

Preserve original block/non-block dictionaries, KwKwK, width growth/alignment,
CLEAR, ignored 0x20/0x40 flags and the original lack of a lower-width header guard.
Unix compress consumes to EOF: no decoded-size promise, checksum or end marker
exists. Incomplete final-code bits are ignored; truncation can produce a valid
shorter stream and appended bytes may become codes. Report one member and zero
trailing bytes; never claim that all truncation/corruption is detectable.

Two concrete native corrections are intentional. The original final callback
can fail while its wrapper returns 196645 success bytes: only 196608 were accepted
with 64-KiB output buffers, or zero with Windows' default 256-KiB buffer. Native
consumer failures propagate their errno, including the final callback, instead
of publishing success. The original also increments saturated nine-bit streams
to ten bits: a valid 4096-literal zero stream silently becomes 3712 bytes. Native
initial and CLEAR resets retain nine bits at that dictionary limit, matching
current upstream ncompress's fixed decoder. Windows preprocessing remains exact.
The fixed decoder and native provider agree on all 24 pinned upstream vectors;
ten additional full-width nine-bit reference vectors and the silent-loss fixture
produce exact content. Legacy ncompress 4.2.4.6's long `-C` output does not round
trip even through its own decoder; use separately verified non-block fixtures.
The old encoder also loses three bytes in the sampled saturated nine-bit stream;
without a format size/checksum that loss cannot be inferred from the stored file.
Provider revisions, source SHA256/URLs and every observed result are retained.

The independent Python fixture encoder is test-only. GNU gzip and both pinned
ncompress decoders validate proper non-block fixtures at supported original
boundaries; fixed current ncompress validates saturated nine-bit fixtures. The
387-case corpus compares native bytes/hash/prefix/count with independent data and
the original wrapper at 64-/256-KiB input/output and gzip-dispatch entry. It covers
all 9..16 widths, block/non-block, saturation, CLEAR/group/refill boundaries,
KwKwK, flags, bad headers/codes, incomplete final bits, mutations/tails, explicit
limits and both native corrections. The retained-source test passes 37197 checks,
all five allocation failures, short/EINTR/early/late I/O, buffered/callback cancel,
final positive/negative consumer errors, identity changes and concurrent readers.
An independent gzip pass verifies all prefix/zero content. The actual native
count separately confirms 4294967808 decoded bytes, exact prefix and caller-bound
refusal, storing only encoded data; its large test uses count mode, not a native
full-output digest. Normal final source/corpus/large tests pass. The final ASan/UBSan source/corpus/large qualification also passes.

The initial 21-target regression set passes on both builds and covers admitted gzip/XZ/BZIP2/LZMA source,
writer, helper/protocol and all 96 analysis/99 focused Widgets results. The final
nine-bit correction is separately rebuilt and requalified on the three provider
and source targets; it cannot alter the unchanged other provider bodies. Record
all terminal results and sanitizer diagnostics under `.rufus-test-artifacts/cp28a`.
The first broad build command named nonexistent test targets and stopped before
compilation; corrected target names build with Ninja default parallelism. Preserve
that invocation error and the pre-correction runs as evidence, not final coverage.

This is source qualification. `.Z` writer/helper/ordinary SELECT and Unix-compress
magic fallback in `.gz` remain gated. Ordinary creation is still gzip/XZ/BZIP2/
LZMA. Next qualify frozen LZW plans/native writer/strict helper, actual guest
claims/fault/readback/bootable restore, then ordinary controller/READY and the
original `.gz` fallback. ZIP/Zstd/VTSI, containers, force-marker UI, saving,
remaining SELECT/drag/drop and all advanced/legacy parity remain in scope.
Remove owned temporary encoded/large fixtures after exporting their observations
and hashes. Retain the current builds and small pinned encoders for the next
LZW writer checkpoint; preserve user sources, recovery USB and old system cores.


## CP28b — LZW frozen plans, native writer and strict helper (2026-10-18)

Admit Unix-compress/LZW (type 2, `.Z`) through the two existing native-writer
and two strict-protocol provider guards. The entire writer and protocol bodies
are identical to CP28a after removing those four guard extensions. CMake adds
three test registrations, with no new production executable or dependency. Five
production files change (including the source-error correction below), none are
added and 725 prior production hashes remain unchanged. Original shared cores,
Windows projects, other providers, production helper/job/claim/cleanup code,
ordinary worker/controller and picker are unchanged.

Freeze retained source identity, association, actual decoded size, prefix/marker,
member/tail facts, sector and decoded SHA256. The same helper owns the source and
exclusive target through four complete decodes (helper preflight, writer
preflight, copy and verification), sync and checked close. Actual kernel logical
sector/capacity are supplied by the native job. The unchanged engine batches at
64 KiB, pads the final sector with zeros, preserves the target tail, checks
positioned short/EINTR I/O, fsyncs dirty failures and reads back the complete
padded image. No expanded temporary image, formatter, extra authorization or
runtime compressor is added. DONE still requires successful I/O/readback; GUI
READY additionally requires successful checked cleanup and joined helper/worker.

Qualification found another inherited source error. After a successful output
callback, the original invalid-code branch reports `corrupted data` but can
return that preceding positive write count. With one/two/three complete output
chunks followed by an invalid code it returns 65536 while accepting 65536,
131072 or 196608 bytes. This could publish an incomplete read as success. Every
native LZW error diagnostic now aborts with EBADMSG across the existing owner
boundary, clearing public source publication; healthy decoding/I/O/ownership
is byte-identical after normalizing those two diagnostic hooks. The original
Windows core remains untouched. Retain the original-wrapper/native observation
and three new differential regression cases. CP28a's final-callback and native
nine-bit corrections remain in force; its shared-core preprocessing and healthy
actual >4-GiB counting evidence are retained without repeating that unchanged
large decode at this stage.

The independent test encoder covers 9..16 bits, block/non-block, saturation,
CLEAR and KwKwK within repeated native writer decodes. A dedicated CLEAR plus
37-literal tail lets the fixture replace its last code with invalid code 511:
preflight refuses without writes; a copy fails after exactly 196608 committed
bytes, syncs once, and leaves the pending final sector untouched. This is an
invalid-code failure, not a fictitious CRC. Valid incomplete-code bits are
ignored, truncation can be a valid shorter stream, and appended complete codes
become output. The plan freezes that actual result. A shortened source supplied
with the old helper size/hash is stale before target acquisition.

Valid literal-code substitutions have identical stored size, association,
decoded size, prefix/marker/member/tail and sector facts, but different decoded
SHA256. The C fixture independently prepares both plans and asserts that only
the decoded hash differs. GNU gzip independently validates the substitute's
content. Preflight returns ESTALE before writes; copy returns ESTALE after
196608 altered bytes and sync; verification returns EBADMSG after 65536 bytes
verified against the original target. Physical padding/tail comparisons are
independent Python readback. All source/target identity, append/readability,
capacity, early/late I/O, partial/final writes, sync, cancellation, allocation
failure and concurrent hash-reader contracts remain exercised.

The strict five-provider protocol rejects mismatched suffix/type, strings and
fractions, malformed size/sector/hash/force fields, unknown fields and legacy
operation confusion. Its single-process response cases include result/error/
cancel, held exit, total/count/phase violations and checked cleanup errors. The
simulated-native helper exercises ordinary owned files with link-only kernel,
UID and UDisks facts: `.Z` is added to its source-descriptor inventory so actual
read-only/no-follow ownership, claim lifetime and source-close order are checked.
No host block device, desktop polkit or physical persistence is qualified here.

Normal qualification passes all 17 targets: 390 LZW source cases and 37197
retained-source checks, 188 writer cases, 57 simulated helper jobs, 33 LZW
protocol results and all 97 image-analysis results. All admitted gzip/XZ/BZIP2/
LZMA writer/helper/protocol regressions pass. The complete ASan/UBSan 17-target qualification also passes.
The new analysis case independently packs a valid marked 512-byte nine-bit LZW
image, proves its backend plan succeeds, then proves ordinary SELECT refuses
it before any decoder worker call. Ordinary creation remains gzip/XZ/BZIP2/LZMA;
`.Z` actual guest/controller qualification and `.gz` magic fallback remain next.
The new writer test explicitly confirms LZW data named `.gz` returns ENOTSUP
without target writes. No previously offered GUI format is removed.

Both initial three-target test attempts failed and are retained. The writer
observed ESTALE from the masked invalid-code result instead of EBADMSG; the
helper close-order assertion exposed the missing test-only `.Z` descriptor
inventory. Protocol cases passed. Correct the source error and fixture rather
than weaken the expectations, then require the complete final suites. Build
with Ninja default parallelism. Evidence is `.rufus-test-artifacts/cp28b`:
scoped originals/current sources/diff, production/body reuse proofs, pinned
provider/CP28a references, exact logs/results and current binary/build hashes.
Remove owned encoded/probe targets after exporting hashes and observations;
retain current builds and small independent encoders for the LZW guest stage.
User sources, the iMac recovery USB and old system cores remain untouched.

Next qualify LZW with the real guest kernel/helper, native claims and source/
write/sync/verify/close failures, poisoned full restore/readback and bootable-image
checks, then ordinary SELECT/controller/joined READY and original `.gz` fallback.
ZIP/Zstd/VTSI, containers, force-marker UI, image saving, remaining SELECT/drag/
drop and every remaining advanced/legacy Windows feature stay in scope. Full
Windows parity remains an active goal.


## CP28c — real guest LZW claims, faults and complete restore (2026-10-18)

Qualify CP28b's unchanged LZW source/writer/helper against actual guest block
devices. All 730 production hashes and both production helper binaries remain
identical. Add a mutually exclusive `--lzw-raw-helper` recipe and a required
pinned `--lzw-encoder` used only in disposable tests. Preserve all old recipe
modes and deadlines. Both normal and ASan/UBSan/leak builds pass their first
complete 50-job runs: five required seed helpers, five raw setup jobs, four
mounted LZW jobs and 36 direct LZW cases. The ordinary GUI and `.gz` magic
fallback remain gated until the next checkpoint.

Guest root credentials, source UID1000 ownership/readability, sysfs/ioctl device
identity and 512-byte logical sectors, native source/home/mount protection,
whole-disk O_EXCL claims, positioned I/O, fsync and checked close are real.
UDisks metadata and counted/held pkexec remain fixtures. Verify competing
exclusive opens while every fault job owns the kernel claim, then reacquire it
after every joined result. Each case checks one launch/result, held launcher
exit, exact physical counts, retained sync failure, descriptor/descendant and
mount cleanup. Refusals preserve the complete target; every partial write
preserves all bytes beyond its actual committed boundary.

Run the six existing guest fault modes: 17-byte partial write, cancellation
after 65536 bytes, full dirty sync failure, readback-buffer corruption, checked
target-close failure after full verification and observed success. Two late
source refill modes return EIO/premature EOF after exactly 65536 bytes, with
one dirty sync and no readback. Extend only the guarded probe for three LZW
read-buffer modes. A final invalid code after CLEAR fails EBADMSG after exactly
1048576 bytes; the pending final sector is untouched. Valid literal-data changes
preserve stream shape, size and prefix: copy returns ESTALE after 1048576 altered
bytes and sync, while verify returns EBADMSG after 524288 bytes verified against
the original copied target. The actual source file is unchanged; these are
controlled in-memory I/O faults, not on-disk source mutations. Eleven observation
jobs record actual source reads, target writes/readback, sync, held claim checks
and one checked target close. All retain the claim through failure cleanup.

Use incompressible 1048613-byte small data to force late refills. Independent
current ncompress produces a 1297147-byte primary stream in both builds. Stage
20 additional deterministic Python-encoded fixtures only in the guest, verifying
each complete output/error first with the pinned fixed ncompress decoder.
Cover block/non-block 9/10/12/16-bit streams, saturation, CLEAR and long KwKwK
patterns; literal streams; incomplete final bits, appended complete codes and
valid truncated prefixes; shortened/wrong-hash wire refusals; short headers,
unsupported widths, initial and late invalid codes. Verify encoded fixture hashes
and independently derived decoded sizes/SHA256 in the guest. LZW has no checksum,
declared size or end marker: truncation can be a valid shorter stream, incomplete
bits can be ignored and appended codes can become image data. No CRC or generic
truncation detection is claimed. CP28a/CP28b's native nine-bit and error corrections
are exercised through the real helper, preserving the original Windows core.

The four mounted jobs cover busy refusal, target-backed caller home refusal,
cancellation after unmount before writes and one successful complete creation.
All later DD jobs run unmounted after replacing the partition table. Before the
final production-helper restore, poison all 67108864 target bytes with 0xA5,
sync and close that seed descriptor. Compress the original raw bootable image,
then restore through the unchanged helper's four complete decoded/hash passes.
Require original raw/frozen LZW/guest/host full SHA256 equality, independent
MBR/FAT/filesystem/files inspection and Syslinux/menu/boot-sector BIOS execution.
Backing SHA256 remains unchanged after boot. Each build records its own full
digest; filesystem IDs/times may differ between runs. This qualifies a bootable
image restore, not installed-OS/kernel boot from the created media.

Host runs remain ordinary UID1000 on private tmpfs, TCG, no network and no host
device passthrough. New LZW bounds are 120 seconds for the encoder, 90 for small
helpers, 600 for the full four-pass restore and 1800 for the whole focused guest.
Keep the existing 15-second progress diagnostics and all older bounds. No new
production timeout, runtime encoder/decoder, authorization or expanded image.
Physical media, desktop polkit, other actual sector sizes, power-loss persistence
and installed-OS boot remain unqualified. Reuse unchanged CP28b source/writer/
protocol/analysis and older provider qualifications instead of repeating their
full matrices. No Windows customization installation matrix is added.

Evidence: `.rufus-test-artifacts/cp28c`, scoped before/current/diff, unchanged
production/helper proofs, driver/probe/encoder/kernel/archive inputs and hashes,
50-job logs/results, 20-fixture manifests, eleven real claim observations per
build, whole-image hashes, independent files/BIOS and cleanup. Export small
evidence and remove all owned audit streams, images, roots, archives, copied
trees and private tmpfs parents. Retain current builds, qualified installer and
small pinned encoder for ordinary LZW/fallback qualification. Preserve user
sources, iMac recovery USB and old host system cores.

Next: ordinary LZW SELECT/picker/controller/joined READY and original Unix-
compress magic fallback inside `.gz`. ZIP/Zstd/VTSI, containers, force-marker UI,
saving, remaining SELECT/drag/drop and every advanced/legacy Windows feature
remain in scope. Full Windows parity stays active.


## CP28d — ordinary LZW creation and original gzip fallback (2026-10-18)

Enable ordinary asynchronous SELECT and confirmed DD creation for Unix-compress
(`*.Z` and `*.z`). Match original gunzip's initial `1f 9d` dispatch for LZW data
named `*.gz`, before allocating gzip state. Preserve filename association 3 for
that fallback while decoding actual LZW semantics. There is no LZW checksum,
declared size or end marker: incomplete code bits can be ignored, truncation
can produce a valid shorter image, and complete appended codes become output.
Real gzip continues to validate every contiguous member's CRC/ISIZE and ignore
its original trailing garbage; LZW magic after a gzip member remains such a tail.

Five of 730 production files change: source dispatch/header, ordinary provider
admission, picker and three CMake test registrations. All other 725 hashes are
unchanged. Original Windows/shared cores, native LZW decoder, writer, protocol,
helper job, claim/cleanup and controller bodies are unchanged. Helper binaries
are relinked for the new gzip dispatch. Reuse CP28c's real direct-LZW fault and
full-target poison qualification, then qualify the new ordinary/fallback route.
No external encoder or decoder is required at runtime; the pinned independent
ncompress executable is used only by tests.

Both normal and ASan/UBSan/leak builds pass all 23 focused host targets. Replay
390 LZW source cases for each filename association; direct `.Z` and `.gz`
fallback source contracts pass 37197 and 46572 checks, with five allocations
and peak tracked allocation of 526688 bytes. Native writer coverage is 188/187
cases, and each association passes 57 simulated helper jobs. Existing real-gzip,
XZ, BZIP2, LZMA, protocol and actual >4-GiB gzip regressions pass. Both final UI
rechecks pass 130 analysis and 135 focused Widgets results. Six representations
include genuine gzip and LZW data named `.gz`; all 30 directed queued selection
pairs, cancellation, geometry 512/4096/65536, capacity, source/sequence/sector
invalidation before authorization, failure and joined READY are exercised.
Independent ncompress decoding verifies the literal-only Qt fixture encoder.

Both complete 17-job real-kernel guests pass: five required seed helpers, five
raw setup jobs, two ordinary 64-MiB controller restores (`.Z` and `.gz` fallback),
and five actual fallback source/data fault jobs. The frontend uses the ordinary
analysis/selection/confirmation/controller path with one launcher per creation.
It stays at 99 while the launcher is held, then reaches full green READY/100
only after helper/worker join; host Widgets tests also cover one notification
and unplug-after-success stability. Raw source, both decoded plans, actual
written/verified disk and independent host full SHA256 agree within each run.
Independent MBR/FAT/filesystem/files inspection and BIOS Syslinux/menu/boot-sector
execution pass; the backing hash remains unchanged after boot.

The five fallback faults hold an actual exclusive kernel claim through dirty
sync and checked close: source read/EOF fail EIO after 65536 bytes, late invalid
code fails EBADMSG after 1048576 bytes, valid copy-data substitution fails ESTALE
after 1048576 bytes, and valid verify-data substitution fails EBADMSG after
1049088 written and 524288 verified bytes. Actual source files remain unchanged;
these are controlled read-buffer faults. Every observation records one sync and
one close, at least five held claim checks, and reacquisition after joined exit.

Host processes run as ordinary UID1000 with disposable tmpfs disks, QEMU TCG,
no network and no host-device passthrough. Guest helper credentials, sysfs/ioctl,
sector 512, source ownership, native positioned block I/O, exclusive claim,
fsync and checked close are real. UDisks and counted/held pkexec are fixtures;
the frontend main thread models caller UID1000 while its worker/helper use real
guest credentials. Other actual sector sizes, physical media, desktop polkit,
power-loss persistence and installed-OS/kernel boot remain unqualified. New
LZW-only bounds are analysis 180s, helper 600s, child 960s and guest 2400s;
existing provider deadlines and production behavior are unchanged.

Preserve initial fixture build failures (mixed enum/int initializer list), then
the passing rebuilds and host suites. The initial normal guest completed all
17 product jobs but its host parser dropped digits in `HELD99`, so readback/BIOS
qualification did not run. Verify the corrected parser against the original
frames. Stop the exact owned sanitizer QEMU that loaded the same defective
parser, preserve both attempts, and rerun both complete guests with the fix.
Only the final complete runs establish host readback and BIOS success.

Evidence: `.rufus-test-artifacts/cp28d`, scoped originals/current/diff, production
reuse proof, exact source/binary/provider/input hashes, host logs, initial
attempts/parser reproduction/controlled-stop record, final guest results,
full-image hashes and cleanup. Remove all owned guest images, archives, roots
and private tmpfs parents after exporting small evidence. Retain current builds,
qualified installer and small independent test encoder for continuing parity.
User sources, the iMac recovery USB and old system cores remain untouched.

Ordinary compressed creation now supports gzip/XZ/BZIP2/LZMA/LZW. Fourteen
original ZIP container diagnostics record first-entry/CDF selection, stored
return/CRC behavior, size/ZIP64 handling and simulated-provider return control
flow in `next-zip-audit`. Its deflate shim is zlib and other method decoders are
explicitly simulated; this is preparation, not ZIP provider qualification.
Next build ZIP's retained-source foundation using that audit. ZIP/Zstd/
VTSI, VHD/VHDX/FFU, force-marker UI, saving, remaining SELECT/drag/drop and every
remaining advanced/legacy Windows feature stay in scope. Full parity stays active.


## CP29a — retained-source ZIP stored/deflate foundation (2026-10-18)

Extract the original Windows ZIP record definitions without changing the
default Windows container preprocessing (93,789 normalized bytes compare
identically). The native positioned reader selects the first central-directory
entry, including a leading directory, as original Rufus does. It bounds EOCD,
ZIP64, central/local records, extras and the compressed slice; refuses split or
encrypted archives; and supports methods 0 (stored) and 8 (raw deflate).
Stored output now reports its actual length and checks CRC, correcting the
original branch's zero count/unchecked CRC. Raw deflate retains the original
first-stream and declared-length mismatch behavior when CRC passes. A short
ZIP64 end-record fixture triggers an original ASan 52-byte overread; the native
sanitized reader returns the independently checked decoded bytes. These native
corrections are scoped and tested. Eight of 730 previous production files
change, three are added and 722 previous hashes remain unchanged.

Both normal and ASan/UBSan/leak builds pass all 28 host targets. The ZIP source
corpus passes 140 final archive/selection/integrity/bounds cases; stored,
deflate and repetitive-deflate contracts pass 38,063, 38,152 and 1,407 checks.
Each build independently verifies complete content, CRC and actual decoded
count for a 4,294,967,808-byte ZIP64 stream without retaining the expanded
image. Previous compression, writer/helper/protocol and 130 analysis/135
Widgets regressions pass. The one sandbox LeakSanitizer ptrace limitation is
reproduced separately; authorized ordinary-user host rechecks pass.

This is a source-only foundation. ZIP methods 12/14/95, frozen plans, writer,
strict helper, real guest, ordinary SELECT/creation and joined READY remain
unqualified and gated. Next qualify the remaining original ZIP methods, then
the write/helper/guest/UI path. Zstd/VTSI, containers, force-marker UI, saving,
remaining selection/drag/drop and all advanced/legacy Windows features remain
in the active full-parity goal. Evidence: `.rufus-test-artifacts/cp29a`.


## CP29b — all original ZIP entry methods in the retained source (2026-10-18)

Extend CP29a's first-central-entry, bounded ZIP/ZIP64 reader with the three
remaining original Windows methods: BZIP2 (12), LZMA (14) and XZ (95). Reuse
the embedded native adapters around Rufus's unchanged original BZIP2/LZMA/XZ
decoder algorithms, with pread confined to the selected compressed slice.
Validate the ZIP entry CRC after each complete decode; compressed methods retain
actual output when declared length differs but CRC passes. BZIP2 joins streams
inside one entry; XZ stops after its first stream. ZIP-LZMA translates its
properties header into the original range decoder's form. General-purpose bit
1 selects EOS or declared-size termination, as specified by
[PKWARE APPNOTE](https://pkwaredownloads.blob.core.windows.net/pkware-general/Documentation/APPNOTE-6.3.7.TXT).
No external decompressor is added at runtime.

The CP28d original-container shim observed a `-14` result for methods 12/14/95
even when simulated decoders reported positive output. Native source publication
corrects that wrapper result after bounded decode, ZIP CRC and retained-source
validation. This is a documented Linux divergence required for coherent output;
the original Windows container and shared decoder cores remain unchanged.
Eight of 733 previous production files change, 725 hashes stay identical.

Both ordinary-user normal and ASan/UBSan/leak 28-target host suites pass,
including ZIP64's independently checked actual 4,294,967,808-byte deflate
output and all prior compression, writer/helper/protocol and 130 analysis/135
Widgets regressions. Final focused ZIP rechecks pass 256 archive cases per
build and all six source contracts, including two-stream BZIP2 and ZIP-LZMA
with and without EOS. No large expanded image is retained. Evidence:
`.rufus-test-artifacts/cp29b`.

This qualifies source reading for the original five ZIP methods, not USB
creation. Next freeze ZIP decoded plans and qualify native writer/strict helper,
then real guest and ordinary SELECT/confirmation/joined READY. Unknown methods,
Zstd/VTSI, containers, saving, force-marker UI, remaining selection/drag/drop
and all advanced/legacy Windows features remain in the active parity goal.


CP29c adds ZIP to frozen decoded SHA256 plans, native padded write/readback and
the strict helper packet while leaving the ordinary Qt selection gate closed.
The simulated helper accepts all five original ZIP methods and checks claim,
cleanup and failure ordering on owned regular files. Four of 733 previous
production files change; ZIP guest block writes and ordinary confirmed Qt
creation/READY are next. Full Windows parity stays active.


CP29d qualifies all five original ZIP entry methods on a real disposable guest
block device through the production helper. Both normal and sanitizer guests
pass 23 ZIP jobs, eight claim observations, complete 64-MiB deflate restore,
independent readback and BIOS boot. All 733 prior production hashes stay
unchanged. Ordinary ZIP worker SELECT/confirmation/joined READY is next;
other Windows parity groups remain in scope.


## CP29e — ordinary ZIP selection and confirmed creation (2026-10-18)

Admit `.zip` and `.ZIP` to the ordinary file picker and ZIP to the asynchronous
image-analysis worker after CP29d's real device qualification. The first central
directory entry supplies the decoded boot marker, size and SHA256 for the same
frozen plan and one-helper DD route as the earlier compressed providers. The
existing helper supports stored, deflate, BZIP2, LZMA and XZ ZIP entries;
the full ordinary controller guest uses deflate. No new runtime ZIP executable
or additional authorization is added.

Normal and sanitizer Qt analysis/Widgets targets pass, including the new picker
case, malformed ZIP refusal, source replacement, sector/capacity and prewrite
identity checks, failure handling and held 99%/joined READY. Both disposable
512-sector QEMU guests pass the 64-MiB deflate SELECT/START/confirmation and
single-helper restore after their five seed and five raw jobs. The decoded,
guest and independent host disk hashes agree; host FAT-file reads and BIOS boot
pass. The sanitizer guest reports no ASan/UBSan/leak diagnostics. Three of the
733 CP29d baseline production files change; 730 hashes stay identical. The
temporary guest media are removed. Evidence: `.rufus-test-artifacts/cp29e`.

ZIP ordinary creation is now qualified for this bounded first-entry policy.
Remaining providers (Zstd/VTSI), VHD/VHDX/FFU, device-image saving, force-marker
UI, other selection/drag-drop behavior, real hardware/sector diversity and the
rest of Windows parity remain in scope. The goal stays active.


## CP30a/b — bundled Zstd source, frozen writer and helper packet (2026-10-18)

Compile the original bundled Zstd decoder on Linux behind a narrow dependency
guard. The native adapter reads the retained descriptor with `pread`, leaves
its offset unchanged, limits the decoder to the original 128-MiB window,
counts actual output, validates checked frames and consumes concatenated and
skippable frames to EOF. Non-Zstd tails and incomplete frames fail. The source
corpus checks an independent CLI encoder, read/cancel/callback faults, checksum,
limits and actual `2^32 + 512` byte output without an expanded image.

Admit Zstd to the existing frozen decoded SHA256/sector plan, padded writer,
verification and strict compressed helper packet. The ordinary Qt selection
gate remains closed. Regular-file writer and simulated helper jobs cover
512/4096/65536-byte sectors, source identity, claim, mounted protection,
partial writes, sync/cleanup and cancellation. Normal and sanitizer focused
regressions pass, including prior gzip/XZ/ZIP source and gzip/ZIP writer/helper
paths. Production uses no external `zstd` executable; the independent test
encoder is a CI test dependency. No physical media is accessed.

Next: real disposable guest block-device helper qualification for Zstd,
followed by ordinary Qt picker/SELECT/confirmed creation and joined READY.
Then continue VTSI, containers, image saving and the rest of Windows parity.


## CP30c/d — real Zstd guest and ordinary creation (2026-10-18)

Stage the independent `zstd` encoder only in disposable guests. The normal and
sanitizer 512-sector QEMU guests each pass 13 direct Zstd helper cases plus
eight actual block-claim observations: frozen digest/size/sector and checksum
refusals, mounted protection, dirty cancellation, write/sync/verify/close and
late source I/O faults, then a fully poisoned 64-MiB bootable restore. The
decoded/guest/host disk hashes agree; ten FAT files read independently and BIOS
boot reports `RUFUS_RAW_BOOT_OK`. Guest media are removed after log export.

With that gate satisfied, add `.zst`/`.ZST` to the picker and Zstd to the
asynchronous image-analysis worker. Both normal and sanitizer analysis/Widgets
targets pass, including picker selection, frozen decoded SHA256, malformed
frames, source changes, geometry/capacity, confirmation, helper failure and
held-99/joined-READY. Both ordinary controller guests select Zstd, confirm the
512-byte target plan, launch one production helper, write and verify the full
64-MiB image, hold 99% until helper exit, then reach 100% READY. Independent
host hashes/FAT reads and BIOS boot agree. The original Windows decoder source
files remain unchanged; only its dependency include shim is guarded for Linux.
No physical USB is used. Evidence: `.rufus-test-artifacts/cp30ab`.

Zstd ordinary creation is qualified on this 512-byte guest route. Continue
VTSI, VHD/VHDX/FFU, device-image saving, force-marker UI and all remaining
Windows Rufus parity features; the goal stays active.


## CP31a — VTSI retained sparse source (2026-10-18)

Add a read-only `.vtsi` provider to the existing retained-source API. The
original Windows decoder checks a 512-byte Ventoy footer and segment-table
checksums, then reads segment data consecutively and seeks the output device
to each specified disk sector. The Linux reader validates version, checksums,
table/data bounds and ordered nonoverlapping disk extents before exposing a
logical image stream. It emits zeros for sparse gaps so analysis sees the full
declared disk size; this does not write or clear any target. As in the original
decoder, the segment `data_offset` field is ignored. The descriptor position
is preserved, read/metadata buffers are bounded, cancellation is checked and
actual logical output is counted in 64 bits.

The independent fixture covers footer/segment corruption, layout and size
limits, short/EINTR/I/O/cancel/callback faults, a table crossing the 1024-entry
batch boundary and a `2^32 + 512`-byte logical image without storing expanded
data. Thirty-seven vectors pass. Normal and sanitizer focused source regressions
each pass 4/4 CTest targets; both app/helper builds link. No physical media is
used. Evidence: `.rufus-test-artifacts/cp31a`.

Next: a frozen VTSI write plan and native sparse-device helper route, with
explicit gap semantics and regular-file/fault tests before any guest or Qt
admission. Full Windows parity remains active.


## CP31b — frozen VTSI sparse writer and helper packet (2026-10-18)

Freeze both the zero-gap logical SHA256 and a separate SHA256 of the ordered
VTSI segment map. The strict helper packet carries both digests; the helper
recomputes them before acquiring a target. Reject segment extents that do not
align to the selected 512–65536-byte target sector. Native writes use positioned
I/O only for the listed extents, matching Windows Rufus's sparse seek behavior:
all target gaps and trailing bytes remain untouched. The write result counts
actual segment bytes, while progress follows the logical image through gaps.
After writes, sync and verify every written extent, rechecking frozen source,
target and layout state before DONE.

The independent regular-file matrix passes 26 layout, alignment, gap,
same-logical/different-map, source-substitution, sync/readback and cancellation
cases. Nineteen simulated helper jobs pass strict packet, claim, mounted
protection, dirty failure, cleanup and gap-preservation checks. The normal and
ASan/UBSan/leak focused suites each pass 11/11 targets, including prior gzip
and Zstd regressions, and both app/helper builds link. No physical media is
used. Evidence: `.rufus-test-artifacts/cp31b`.

Next: qualify real VTSI writes and untouched gaps on a disposable guest block
device, then admit `.vtsi` to ordinary Qt SELECT/confirmation/joined READY.
Full Windows parity remains active.


## CP31c — real guest VTSI and sparse client completion (2026-10-18)

The disposable USB guest now exercises a wrong-map prewrite refusal, a two-
extent sparse write with poisoned gaps, real exclusive block claims, source and
target reads, sync and close, then a full bootable VTSI restore. Independent
readback verifies all untouched gaps and the restored 64-MiB disk/FAT files;
BIOS executes the restored image. Both normal and sanitizer guests pass.

The guest found that the Qt helper client had treated VTSI logical progress
as physical byte counters. The raw-result validator now distinguishes VTSI:
logical copy and verify progress must reach the declared disk size, while
physical bytes written must be nonzero and exactly match bytes verified.
Dense routes retain their exact full-size checks. Eight normal and eight
sanitizer helper-process suites pass, including sparse incomplete and
overcount refusals. Evidence and cleaned guest inventory are in
`.rufus-test-artifacts/cp31c`.

Next: admit `.vtsi` to ordinary Qt SELECT, freeze its logical and map hashes,
confirm the write geometry, and qualify single-helper joined READY in normal
and sanitizer guests. Full Windows parity remains active.


## CP31d — ordinary VTSI Qt route (2026-10-18)

The picker and asynchronous image worker now admit `.vtsi` and `.VTSI`, then
freeze the logical and extent-map SHA256 values. The raw plan preserves both
through native geometry confirmation and the strict helper packet. The Qt
controller uses logical VTSI progress for its bar and actual segment counts
for verified completion. It reaches READY only after a clean, joined helper
exit; the completion log reports the physical segment count.

The normal and sanitizer Widgets image-analysis and raw-write suites each
pass 2/2. Both focused real-block QEMU guests pass sparse Qt gap/tail
preservation, one helper per job, held-99/joined-100 completion, a complete
bootable Qt restore and independent host readback/BIOS boot. Direct sparse
claim/fault qualification remains recorded at CP31c. Disposable guest media
is removed after exporting evidence to `.rufus-test-artifacts/cp31d`.

Next: choose another remaining Windows image format or image-saving path from
the parity backlog. Full Windows parity remains active.

## CP31e — native fixed/dynamic VHD foundation (2026-10-19)

The Linux source now validates VHD footer/header checksums, geometry, BAT
bounds, allocated extent nonoverlap and sector bitmap presence. Fixed disks
read their payload before the footer; dynamic disks reconstruct sparse zero
sectors and blocks. Differencing disks return ENOTSUP until a safe parent
chain is implemented. The retained descriptor is never repositioned; decoded
bytes remain provisional until source identity is rechecked.

The native write plan admits VHD and freezes the decoded SHA256. The existing
regular-file writer then copies, syncs and verifies the logical disk. Forty-seven
independent layout/fault/QEMU/readback cases pass in normal and sanitizer
builds; existing source, VTSI and writer regressions pass. Evidence is in
`.rufus-test-artifacts/cp31e`.

Next: qualify the strict device helper and real guest route, then ordinary Qt
selection/READY before adding `.vhd` to the picker. VHDX/FFU, differencing
parent chains, image saving and the rest of Windows parity remain active.

## CP31f — VHD strict helper and real guest (2026-10-19)

The VHD request now passes strict decoded-size and progress validation in the
device helper. Normal and sanitizer process suites exercise malformed packet,
wrong source, short/overcount and cleanup behavior. Thirty-four simulated
fixed/dynamic device cases cover claims, write/read faults and preservation.

Both disposable QEMU guests reject a changed decoded SHA256 before writes,
verify a 256-KiB dynamic disk under a real kernel claim, then restore the
complete 64-MiB bootable disk from a fixed VHD. Independent host disk/FAT-file
hashes and BIOS boot pass. Large guest media is removed after evidence export
to `.rufus-test-artifacts/cp31f`. Ordinary Qt selection/READY and the rest of
Windows parity remain active.

## CP31g — ordinary fixed/dynamic VHD Qt route (2026-10-19)

The ordinary picker now selects `.vhd`/`.VHD`. Its worker analyzes fixed or
dynamic VHD, freezes the logical disk SHA256 and carries it through target
geometry confirmation to the production helper. Normal image-analysis/full
raw-write Widgets, sanitizer image-analysis/focused VHD Widgets and both
production application links pass.

Both disposable QEMU guests pass a prewrite changed-hash refusal, one
256-KiB dynamic Qt job with target tail preserved, and one complete 64-MiB
fixed VHD Qt restore. Each Qt job uses a single helper, holds 99% until exit,
and reaches READY only after full write, verification, sync and joined exit.
Host disk/FAT-file readback and BIOS boot pass. Evidence is in
`.rufus-test-artifacts/cp31g`; large guest media was removed.

Next: VHDX/FFU, differencing VHD chains, image saving and the remaining
Windows parity backlog. The goal remains unfinished.

## Next qualification batch — VHDX and FFU

Implement the two remaining container readers against the existing frozen
logical-image source and native DD writer. Keep focused malformed-container,
source-change and size tests for each reader. Use the already qualified shared
helper fault suite unless the protocol or writer changes. After both readers
pass local checks, run one combined normal guest and one combined sanitizer
guest: each format gets a real-device write/readback case; one full bootable
restore checks the shared writer and BIOS handoff. Run focused Qt selection
cases during implementation, then one full Qt regression at admission. Record
one checkpoint for the completed batch, not separate checkpoints for source,
helper, guest and Qt. Recheck whether this batch is the highest-value next step
against the complete backlog before starting it. Image
saving and VHD parent chains need their own design because their source and
output rules differ.

## CP32 — first installable Linux preview (2026-10-19)

Deliver a usable application milestone with the existing qualified media
routes. The Release build uses the upstream version from `rufus.rc` and a
Linux preview revision. About shows the original GPL and credits; help and
version work without a graphical session. CMake now produces a CPack install
archive and a native Arch/CachyOS recipe, with the GUI, matching helper/policy,
both selected Syslinux versions, extlinux, icons, license and support documents.

The package uses `/usr/bin/rufus-linux` and private helpers under
`/usr/lib/rufus-linux-preview/`. Package inspection checks the policy path,
installed executables, runtime resolution, desktop entry and headless CLI.
The package is built and staged without installing it on the host. Media writer
code is unchanged; its existing guest evidence is retained rather than rerun.
The [release plan](linux-release-plan.md) now defines preview delivery, grouped
feature completion and final parity signoff. Full Windows parity stays in scope.

## CP33 — fixed/dynamic VHDX and preview 2 (2026-10-19)

Deliver clean fixed/dynamic VHDX end to end in one batch: native retained-source
reader, decoded identity/geometry, ordinary picker/worker, confirmed single-helper
DD, readback and joined READY. The reader checks MS-VHDX CRC-32C headers and
regions, required metadata, BAT bounds and overlapping extents before output.
It uses the existing writer/protocol and adds no production QEMU dependency.
Windows uses its virtual-disk provider; Linux currently refuses pending logs
and parent chains instead of replaying or resolving them implicitly. Those
remaining features stay in scope. The source is never modified.

The 40-case source/native-write corpus passes normally and under ASan/UBSan.
One combined guest per build writes a small dynamic image and restores a full
bootable fixed image through the ordinary Qt flow. Each guest uses one seed
job and two VHDX Qt jobs, with existing helper fault evidence reused. Independent
disk/FAT-file readback, BIOS boot, held-99 progress and joined READY pass.
Normal image-analysis and full raw-write Widgets suites pass once at the batch
gate. Large disposable media is removed; compact evidence is retained under
`.rufus-test-artifacts/cp33`.

Preview 2 includes this route in native Arch/CachyOS and generic install packages.
One checkpoint covers implementation, qualification and delivery. FFU remains
the next container gap; VHD/VHDX parent handling, VHDX log replay, device-image
saving and the rest of the parity backlog remain unfinished.

## CP34 — full FFU and preview 3 (2026-10-19)

Connect native full single-store V1/V2 and V1-compressed none/plain-XPRESS
reconstruction to ordinary SELECT/confirmation and the existing DD helper.
Freeze the captured logical size/SHA256; validate every container chunk before
output. Repeated destinations use last-write order and end-relative entries
resolve at the captured size. This route preserves the captured partition
bytes; DISM's portable resizing, other codecs/stores/partial updates and catalog
trust remain explicit parity gaps, detailed in the support guide.

Qualify this as one batch: the focused parser/mapping/codec/integrity corpus,
independent V1/V2 encoder and XPRESS decoder comparisons, one combined guest
per build and one normal image-analysis/full raw-write Widgets gate. Reuse the
existing helper faults. Preview 3 packages the route. Keep compact evidence in
`.rufus-test-artifacts/cp34` and remove disposable images after export. Continue
with device-image saving and remaining image operations under the release plan.

## CP35 — device saving and preview 4 (2026-10-19)

Deliver fixed VHD, dynamic VHDX and full uncompressed FFU capture together:
ordinary save dialog, one read-only UDisks acquisition, bounded encoding,
decoder/hash verification, source consistency reread, atomic destination
publication, cancellation and joined READY. Keep the UI unprivileged and the
source exclusive through verification; require unmounted eligible media.
The existing destination stays in place until the verified image commits.

The focused normal/sanitizer corpus and combined guest route share one
representative VHDX helper restore and BIOS boot; reuse unchanged writer fault
qualification. Independent QEMU/Img2Ffu readers verify outputs. Package the
complete action as Preview 4 and keep one compact CP35 evidence record.
Windows ISO saving exports a mounted tree as UDF and remains the next distinct
saving workflow. Remaining FFU capture layouts, source unmount/read-only
discovery, FFU variants/resizing/trust, parent/log handling and the full parity
backlog remain required. See the support guide and CP35 test record.

## CP36 — ISO export, source unmount and preview 5 (2026-10-19)

Deliver UDF-only filesystem export and automatic source unmount together.
Reuse CP35 capture/transaction/READY behavior and the existing protected native
unmount checks. Use one authorized source helper for mounted captures and ISO
private read-only mounts, with descriptor transfer to the unprivileged GUI.
Keep the encoder and output unprivileged, verify the entire ISO/source manifest
and join source cleanup before publication. Offer ISO in the ordinary save
picker when discovery supplies filesystem metadata. Retain unsupported tree
and filesystem bounds explicitly; never force a busy unmount.

Qualify one combined normal/sanitizer guest route with FAT and NTFS-3G ISO,
mounted VHDX and cancellation/failure cleanup; use kernel UDF readback and one
representative native VHDX restore/boot. Batch focused source/export tests and
one relevant Qt gate, reusing unchanged writer fault evidence. Package the
complete action as Preview 5 and retain one compact CP36 evidence record.
Read-only discovery and remaining image operations continue under the full
Windows parity backlog; no full parity completion is claimed.

## CP37 — read-only discovery and saving, preview 6 (2026-10-19)

Deliver read-only USB discovery and all four existing image-save formats in
one batch. Make read access explicit in source admission and retain default
write refusal. Preserve known capability transitions without replacing source
identity; invalidate creation jobs when access becomes read-only and exclude
unknown state. Show the source capability in the Device list and keep save
available while START and formatting are disabled. Never clear write protection
in production. Reuse existing encoder, atomic-output, cleanup and READY paths.

Batch discovery/mount-policy and write-refusal checks with normal/sanitizer
capture tests. One combined guest per build qualifies all formats from a
kernel-read-only source, with one representative saved VHDX restore/boot and
independent QEMU/UDF readback. Reuse CP35/36 payload and fault evidence. Run one
Qt regression gate, deliver Preview 6 and retain compact CP37 evidence while
removing disposable media. Continue with remaining container/image operations
and the complete Windows feature backlog; this is not full parity signoff.

## CP38 — parent images and VHDX replay, preview 7 (2026-10-19)

Deliver portable VHD/VHDX differencing chains and VHDX version-0 log replay in
one batch. Retain every caller-owned parent source throughout the helper job,
validate linkage/geometry/identity, and include each backing in native target
protection before unmount and after claim. Reconstruct VHD MSB and VHDX LSB
sector ownership. Replay the active circular log into an immutable bounded
read overlay before regions, metadata or BAT reads, with zero/data updates,
overlap ordering and virtual extension. Reuse the frozen logical hash, native
verified DD writer, one authorization and joined READY.

Use one normal/sanitizer corpus and one combined guest per build: small VHD
and VHDX children, parent ownership/target refusals and one full parent-plus-log
restore with host file readback and BIOS boot. Run one Qt gate, reuse the
unchanged writer fault matrix, package Preview 7 and retain compact evidence.
Document portable relative locator support, immutable-source behavior and
resource limits; Windows-specific relocation and other full parity gaps stay
in scope.

## CP39 — FFU codec/descriptor/capture batch, preview 8 (2026-10-19)

Close the missing LZNT1 and Huffman-XPRESS decoders, bounded grouped compressed
buffers, multi-block end-relative runs and FFU capture name restrictions as
one delivery. Preserve source identity and full decoded chunk hashes, last-write
ordering, sparse zeros and the unchanged verified native writer. Retain the
16-MiB group/64-MiB table budgets. Full FFU captures retain exact GPT bytes and
omit optional manifest names for unnamed/non-ASCII GPT names; reject INI line
controls and invalid UTF-16. Use independent codec/capture comparisons.

Qualify one grouped normal/sanitizer corpus, one combined LZNT1/Huffman guest
per build with one representative bootable restore, and one Qt regression gate.
Reuse writer/helper failure, source acquisition and transactional publication
evidence. Deliver Preview 8, one compact evidence record and remove disposable
media. Target-aware resizing, partial base validation, multi-store deployment,
SFU and catalog signer trust remain in the explicit FFU backlog.

## CP40 — ordinary FFU deployment on larger disks, preview 9 (2026-10-20)

Apply full single-store FFUs as final sparse extents at actual target capacity.
Retain raw descriptor semantics until capacity is known; resolve beginning/end
runs and ordered partial overlaps, preserve gaps, and relocate validated GPT
metadata without partition/filesystem resizing. Freeze a generic image layout
digest through SELECT, confirmation and the authorized helper. Keep the shared
claim, source protection, fsync, readback, cleanup and joined READY boundary.

Use one grouped normal/sanitizer corpus, focused sparse transfer faults and one
combined guest per build covering both codecs, larger GPT deployment and one
representative bootable restore. Run one relevant Qt gate after builds, package
Preview 9, retain compact evidence and remove disposable media. Continue with
optimized resizing, partial/multi-store deployment, signer trust, capture
layouts and full Windows parity. Audit Windows UI exposure before adding extra
DISM features: `/SFUFile` is not passed by Rufus and is not a parity requirement.

## CP41 — exact-sector FFU capture, preview 10 (2026-10-20)

Remove block-aligned captured geometry and the fixed 128-GiB capture ceiling in
one batch. Reconstruct source extents and gaps at exact manifest capacity.
Capture full beginning blocks plus a full end-relative tail, with unique-byte
source hashing/progress and overlap consistency. Share a pure geometry planner
between output preflight and encoding; increase chunk size within the reader's
fixed table/buffer budgets and document the remaining dense bound.

Use one grouped normal/sanitizer corpus and one combined guest per build:
real read-only non-aligned block capture, ordinary Qt FFU restore/GPT readback,
and one representative full bootable restore. Reuse unchanged authorization,
publication and source/claim/writer fault matrices. One relevant Qt gate and two
Preview 10 packages complete the batch. Retain compact evidence and remove
large media/staging. Continue with all FFU and full Windows parity requirements.

## CP42 — live USB persistence, preview 11 (2026-10-20)

Deliver the original persistence controls, frozen size/ext preference, main /
persistence / optional UEFI:NTFS layout, ext2/3/4 formatting and ordered
Debian/Casper config changes as one batch. Size selection reserves source space,
uses production geometry, resets on a new source and clamps on target/format
changes. Persistence disables DD. Root preflight repeats source eligibility.
The helper formats persistence before the main volume, with one authorization;
Debian's / union file is created from a private seed and read back with libext2fs.
Track-rounded extents revealed e2fsprogs's small-final-group reduction; exact
block/reserve expectations now follow initialize.c without large allocations.

The focused corpus covers GPT/MBR ordering/types/attributes, malformed requests,
source refusal before target access, configuration priority/UTF encodings,
format/seed readback and failures, controls/reset/clamping and frozen selection.
Normal and sanitizer components and one combined guest per build qualify each
ext version with a real device write/readback, a GPT/NTFS/ext4/UEFI:NTFS backend
case, formatter failure, single authorization and joined READY. One final
Syslinux Linux-kernel fixture boots twice and verifies a surviving file. This
checks the native workflow and boot arguments, not official-distribution
overlay compatibility. Reuse the unchanged source/claim/helper fault matrices.

One relevant Qt regression gate runs for the complete batch. Export compact
CP42 evidence, two Preview 11 packages and checksums, then remove disposable
media/staging. E2fsprogs is now a required package runtime dependency. Full
Windows parity, remaining FFU work and the other backlog groups remain active.

## CP43 — partial FFU updates, preview 12 (2026-10-20)

Close conditional single-store FFU deployment in one batch. Parse the packed
validation region before write descriptors for V1/V2 and every supported
compressed codec. Bound and hash it with the existing frozen layout. Compare
all declared existing-device ranges before any writes; repeat source identity
and target checks afterward. Preserve omitted bytes, resolve end-relative maps
at the selected capacity and apply partial GPT metadata exactly as declared.
Readback checks new bytes without reapplying old-base constraints. FFU marker
admission matches Windows Rufus's signature-based dispatch.

One 61-case normal/sanitizer corpus covers 512/4096-byte sectors, every supported
version/codec, multibuffer and overwritten comparisons, late wrong-base refusal,
I/O/cancellation/source/capacity faults, malformed/hash failures, larger targets
and declared GPT changes. Reuse full FFU/parser/deployment regressions and the
unchanged source/claim/helper matrices. One combined guest per build performs
a full restore, a zero-write wrong-base refusal and a sparse partial update;
independent whole-disk/FAT readback and one representative BIOS boot qualify
the shared writer. Ordinary Qt jobs retain one helper and joined READY.
The guest exposed a premature COPY event on base refusal; delay that event
until validation passes so the strict client receives the correct PREPARE error.

Run one relevant Qt gate, inspect native and generic Preview 12 packages without
host installation, retain compact CP43 evidence and remove disposable media and
packaging stages. Optimized FFU resizing, multiple stores, signer trust, remaining
capture layouts and all other Windows parity groups remain active.

## CP44 — complete image-entry batch, preview 13 (2026-10-20)

Deliver saved Alt+M marker override, content-based raw recognition, `.usb`/`.wic`
and `.bzip2`, local first-file drops, `-i`/`--iso` and positional startup paths,
and desktop file arguments together. Alt+M affects new selections; accepted
sources freeze it through analysis, confirmation, strict typed raw/compressed
packets and privileged preflight. Omitted override remains false. Sector/WIM,
source identity, target, authorization, sync, readback and joined READY checks
remain required. Drops use copy semantics and existing busy/cancellation gates.
Raw `.iso` data without an ISO tree is admitted by inspected signature, matching
Windows. Add the `.bzip2` alias to the shared decoder because Windows already
advertises it in SELECT; its decompressor is unchanged.

Use one focused Qt corpus for raw/compressed/Unicode paths, saved/frozen flags,
first-file drops and busy/remote/move-only refusal, plus strict protocol cases.
One combined guest per build covers zero-write default-marker refusal, actual
raw and gzip override writes/readback and one representative ordinary raw boot.
Reuse the existing source/claim/helper matrices. One relevant Qt gate completes
the batch; inspect Preview 13 Arch/CachyOS and TGZ packages without installation,
export compact evidence and remove disposable media/stages. FFU optimized
resizing, multiple stores, signer trust, standalone WIM/ESD and the other parity
groups remain active.

The batch gate caught raw-plan suffix bypass for a known virtual container.
Require the validated compressed/virtual-disk route before admitting that path,
including in privileged raw preflight. `.usb`/`.wic` and unknown raw suffixes
still use inspected bytes. Recheck that existing refusal row and the updated
shared `.bzip2` policy after the one full gate; reuse the unaffected guest write
semantics and avoid repeating the full GUI corpus.

## CP45 — full/fast device zeroing, preview 14 (2026-10-20)

Deliver the Windows Alt+Z/Ctrl+Alt+Z operations through the existing native raw
controller, confirmation, one authorized helper and joined READY. Freeze the
selected target and validate a strict source-free operation. Preserve loaded
image selection. Zero jobs skip image ownership/open checks while retaining
native target/home/system/read-only, geometry, unmount and claim checks.
Full mode writes zeros; fast mode follows WriteDrive's 32-MiB uniform zero/FF
comparison and 15-region write backoff, filling nonblank regions with FF.
Bounded native I/O records skipped regions, verifies the complete result and
reports actual physical counters independently of full-capacity progress.
Already blank fast jobs must complete with zero writes after sync, readback,
partition reread, descriptor cleanup and joined helper exit.

Use one 38-case normal/sanitizer corpus for patterns, exact tails/backoff and
transfer/sync/cancellation/staleness faults. Focused Qt/strict protocol cases
cover source-free requests, original shortcuts and zero-write held completion.
One combined guest per build performs mixed fast, all-blank fast and full zero
jobs, then one ordinary bootable restore with independent disk/FAT readback and
BIOS boot. Reuse unchanged source/claim matrices; run one relevant Qt gate.
Package and inspect Preview 14 Arch/CachyOS and TGZ artifacts, export compact
evidence and remove disposable disks/stages. Fast skip maps have a 64-MiB
budget; full mode needs no map. Full formatting, bad-block/fake-capacity checks,
FFU resizing/trust and the complete Windows parity backlog remain active.

## CP46 — compressed FFU capture, preview 15 (2026-10-20)

Save full single-store FFUs with bounded LZNT1 compression. Incompressible
4-KiB chunks use the native literal form; empty regions compress without being
omitted from the restore map. Every original sector, including opaque GPT names,
is retained. Text-manifest names that cannot be represented safely are omitted.
Capture still verifies the decoded image and rereads the source before atomic
publication. Output preflight reserves the worst-case literal overhead.

Use one grouped normal/sanitizer corpus with independent LZNT1 decoding,
exact-sector captures, dirty-target readback and publication failures. Reuse
existing partial/deployment/source/helper evidence. One combined guest per build
captures a read-only block source, restores onto poisoned media, checks preserved
gaps/GPT relocation, and performs one representative full restore/FAT readback/BIOS
boot. Run one Qt gate and inspect both Preview 15 packages; remove disposable
media and package stages. Full formatting remains staged with its GUI disabled
until a separate qualification batch. Optimized FFU resizing, multi-store
selection/deployment, catalog signer trust and the full parity backlog remain
active.

## CP47 — full and quick formatting, preview 16 (2026-10-20)

Enable the existing Quick format checkbox and freeze its choice with the job.
Match Windows's quick-only large-FAT32 rule and cleared/disabled DD checkbox,
while retaining the choice across DD selection. Full FAT clears and verifies
only the selected volume under its own exclusive partition claim, then hands
off to the native formatter for metadata. Native NTFS full formatting
selects --full on ntfsprogs-plus or removes -Q for NTFS-3G's full default after
bounded capability checks. Unsupported quick-only tools fail before partitioning.
exFAT uses native -f with check-written/no-discard; ext3/ext4 disable lazy journal
initialization, including persistence. ext2 retains Windows's equivalent full/
quick behavior. Full format does not imply a whole-disk erase outside the volume.

Remove the 120-second absolute formatter deadline only for full jobs. Preserve
cancellation, TERM/KILL escalation, reaping, sync, metadata verification, one
authorization and READY after cleanup/helper exit. FAT stops on a zeroing/readback
I/O error instead of marking a bad cluster and hiding incomplete verification;
Windows's formatter can mark unusable clusters. This is a deliberate Linux
refusal. Independent bad-block/fake-capacity modes remain separate parity work.

Use one 31-case normal/sanitizer corpus for seven filesystem modes, dirty free
space/journals, bounded nonzero/odd extents, faults and long-runtime supervision.
Native old-format/zero regressions and focused protocol/Widgets cases share the
batch. Combined normal/sanitizer guests exercise all seven native formatters,
four complete helper jobs with joined exit/one authorization, independent ext
readback and one ordinary bootable restore/FAT readback/BIOS boot. Run one Qt gate
and inspect Preview 16 native/generic packages. Reuse unchanged source/claim and
shared helper fault matrices; export compact evidence and remove media/stages.
The NTFS-3G default-selection branch is simulated with native ntfsprogs-plus data
creation; actual NTFS-3G full formatting and physical media are not newly qualified.
The full Windows parity backlog stays active.


### CP50 — extended FFU capture layouts (2026-10-20)

Remove the single-chunk manifest assumption and empty-GPT capture refusal. Align
manifest/store regions independently, retain every chunk hash, and share the
4-MiB GPT table budget with larger-target relocation. Make chunk selection
independent of manifest length so pre-acquisition destination space planning
bounds the actual capture. Keep source/target/helper/READY semantics unchanged.
Use one grouped native/sanitizer layout corpus, reuse existing FFU/atomic-output
regressions, one combined guest per build and one Qt gate. Record Preview 19
packages/evidence and clean disposable media. Optimized FFU resizing, multiple
stores, signer trust and the complete Windows parity backlog remain required.


### CP51 — old-BIOS compatibility batch (2026-10-20)

Enable the original checkbox for eligible BIOS/MBR routes and freeze it with
creation. Use shared start/main/trailing rules, final 0xEA track and UEFI:NTFS/
persistence priority. Pass it to every native MBR option, including payload/SBR
and final WinPE checks; keep filesystem/cluster choice on the same geometry.
Set libfdisk's first-LBA limit for the legacy start. Reuse SBR fit preflight,
claim/sync/readback/cancel/cleanup and one authorization. Group native layouts,
focused protocol/window cases, combined normal/sanitizer guests, one Qt gate and
Preview 20 packages. Keep the complete Windows parity goal active.


### CP52 — ISO → ESP batch (2026-10-20)

Enable original hybrid/non-hybrid/persistence eligibility, default ISO selection
and cancellation. Freeze the ESP choice, revalidate fresh source facts, write/check
main GPT ESP metadata and retain persistence. Preserve selected layout units while
using formatter defaults and Windows' FAT32 ESP quick behavior; show the actual
format in confirmation. Keep all target/source, one-authorization and joined READY
rules. Batch shared/file/protocol/Widgets checks, four actual Qt creations per
normal/sanitizer guest, representative UEFI handoff, one Qt gate and Preview 21
packages. Keep complete Windows parity and native geometry/architecture work active.


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

## CP55 — optimized FFU deployment, preview 24 (2026-10-20)

Optimized full FFUs now fit smaller or larger targets using the authenticated
manifest's expandable NTFS partition. Preserve the captured logical size/hash
for source identity, derive a separate minimum target size, freeze both in the
request, and rederive actual geometry in the authorized helper before writing.
Match manifest partitions to GPT GUIDs rather than assuming contiguous slots.
Shift following descriptor runs in 128-KiB units, align their boundary to 1 MiB,
and update both GPT tables/headers and the protective MBR. End-relative runs
remain tied to the actual device end. The ordinary and partial FFU paths retain
their existing behavior.

After sector readback, reuse checked kernel partition nodes and the existing
private NTFS mount/claim lifecycle. Locate Windows through
`Windows/System32/ntoskrnl.exe`; update matching-disk WinreLocation/ImageLocation
offsets in the OS ReAgent.xml and copy it over an existing recovery XML. Preserve
unrelated XML content. Update recovery while the volumes are clean, then grow
NTFS without moving its start. Validate allocation before/after growth, verify
boot geometry/backup, sync and close every child, mount and target before READY.
Use ntfsprogs-plus ntfsresize, with NTFS-3G's compatible utility as fallback.
Never use the backwards-growth `--expand` operation. A pre-existing dirty or
corrupt inspected NTFS volume is refused: Linux cannot safely attribute that
state to this job. Only the successful resize's own first-boot consistency flag
is allowed at postcheck and is retained for Windows. This is an intentional
Linux safety difference from Windows-managed volume mounting/resizing.

Qualification is one grouped native corpus (45 normal + 45 sanitizer cases),
the 60-case ordinary deployment regression, combined normal/sanitizer guests
(two optimized target sizes plus one representative full restore/readback/BIOS
boot per guest), one Qt gate and two inspected packages. Source codecs, catalog
policy and the existing helper fault matrix are reused. The first guest setup
omitted the NTFS module, then lacked udev links for GPT slots 4/8; both setup
failures were fixed without weakening production checks. Sparse independent
fixtures avoid hashing/restoring unnecessary free blocks. Compact evidence is
in `../.rufus-test-artifacts/cp55/`; see the testing record for final gate results.

At CP55 automatic multi-target/UEFI-LUN mapping was planned. The
[CP56 scope audit](linux-ffu-parity.md) corrects that mistaken requirement and
closes FFU feature implementation. The full Windows parity goal remains active.

## CP56 — FFU closeout, preview 25 (2026-10-20)

Close FFU feature implementation after auditing the actual Windows Rufus
`/Apply-Ffu` and `/Capture-Ffu` entry points and the checked desktop provider.
The earlier automatic multi-target/UEFI-LUN requirement was incorrect: that
provider opens the selected drive and rejects multiple stores. Preserve Linux's
explicit source-store chooser as an extension.

Correct saved FFUs to plain XPRESS (format 3), which the checked desktop
provider accepts. Previous LZNT1 captures remain readable. Bound the encoder's
history, output expansion and cancellation; account for worst-case expansion
in capture planning. Accept standard terminal flags after complete 32-token
groups without accepting malformed trailing data.

The [FFU audit](linux-ffu-parity.md) records the source/provider call chain,
compatibility correction, exact profile and qualification limits. One combined
normal/sanitizer native and guest gate plus one Qt regression gate qualifies
this closeout. Reuse existing unchanged helper/finalization faults. Compact
results and package checks: `.rufus-test-artifacts/cp56/checks.json`.

FFU leaves the feature backlog. The complete Windows parity goal remains active
for supplementary ZIP loading, standalone WIM/ESD, Windows To Go and the other
[remaining work groups](linux-feature-parity.md).

## CP57 — supplementary ZIP archives, preview 26 (2026-10-20)

Ctrl+SELECT attaches an additional ZIP after image selection. The native helper
validates all names/sizes/CRCs before target access, protects both source backing
filesystems and copies the archive after normal creation in the same authorized
job. Repeated names overwrite in central-directory order. Source identity,
private mounts, final sync, strict progress/results and READY after helper exit
are preserved. DD/ext eligibility and clearing/reselection match Windows.

One grouped native normal/sanitizer corpus qualifies ZIP64, all five shared
codecs, Unicode/legacy names, path protection, replacement and I/O/cancellation/
source/sync faults, alongside the existing ZIP DD regression. Combined guests
qualify ordinary Qt formatting, bad-CRC/source-on-target prewrite refusals and a
bootable Syslinux ISO overlay with independent file readback and BIOS boot. The
nine-suite Qt gate runs once; unchanged helper fault evidence is reused.

The [ZIP implementation guide](linux-supplementary-zip.md) records the Windows
entry points, eligibility, limits and the explicit FAILED-on-incomplete-copy
policy. Evidence/packages: `.rufus-test-artifacts/cp57/checks.json`. The full
Windows parity goal remains active for the other backlog groups.

## CP58 — Windows To Go and standalone WIM/ESD (2026-10-21)

Preview 27 closes Windows To Go and standalone WIM/ESD as one batch: ordinary
archive/edition selection, native NTFS metadata including EA/EFS, GPT ESP/MSR or
MBR BIOS/UEFI, BCD/boot files, installed SAN/S-mode and one-helper joined READY.
Normal/sanitizer components cover 284 cases per build. Combined guests cover all
four source forms and one native EFI handoff per build; one full Qt gate passes.
Windows execution/installation and decryption remain deferred. See the
[To Go guide](linux-windows-to-go.md) for precise coverage and evidence. Full
Windows parity remains active outside this completed feature group.


## CP59 — desktop storage, startup and logging (Preview 28, 2026-10-21)

One [desktop batch](linux-desktop-settings.md) implements the original portable
INI selection/key behavior, shared normal/portable preference backend,
cache/report paths, startup extra-devices/filesystem/wait controls, per-user
instance lock, remembered advanced panels and automatic/persistent session logs.
Ctrl+L/P and Alt+R/U match their Windows roles. Full-session exports use a bounded
visible tail and private temporary spool. The accepted idle-close path saves
logs only after existing operation/cancellation guards.

The original Windows references are `src/rufus.c` startup, message shortcuts,
advanced-panel handlers and exit log save, `src/settings.h`, and
`src/stdio.c:SizeToHumanReadable`. Native differences and their reasons are
recorded in the desktop guide. No elevated settings provider is introduced.
This batch reuses existing device, helper, polkit, failure and boot evidence;
normal/sanitizer desktop checks and one full Qt gate cover the changed behavior.
Full Windows parity remains the active goal.


## CP61 — runtime UEFI validation, Preview 30 (2026-10-21)

The [runtime validation batch](linux-runtime-validation.md) closes the session
checkbox, streaming/source checksum maintenance, explicit Windows/SWM change
reporting and original-loader/validator installation together. Shared Windows
eligibility remains in the original build. One strict optional request flag
reuses the same authorized helper and cleanup/exit gate. Normal/sanitizer
integration, combined native guests/EFI corruption controls and one full Qt
gate supply delivery evidence; no extra Windows installation matrix is added.
Secure Boot/revocation, other firmware/architectures and all unresolved backlog
rows remain in the complete Windows parity goal.


## CP62 — bundled Secure Boot metadata/revocation checks (2026-10-21)

Native bounded PE/certificate/DBX/resource parsing, shared fallback policy and
existing OpenSSL/hash code implement the original issuer metadata and ordered
revocation rules. The retained-source analysis worker reports results; START
uses the original Cancel/continue warning before any authorization. No helper
protocol or device-write semantics change. The [guide](linux-secure-boot.md)
and checkpoint receipt record grouped evidence and qualification limits. Remote
policy updates, broader chain/firmware support and the rest of the Windows
parity backlog remain active.


## CP63 — remote policy refresh and atomic DBX cache (2026-10-21)

One [policy update batch](linux-policy-updates.md) implements original session
SBAT/certificate refresh, scheduled DBX timestamp checks, one download decision,
validated atomic native cache, settings/manual/cancel controls and frozen
analysis policy. The existing signed-download API shares the bounded HTTP
transport and retains detached signature checks. No helper/write contract is
changed. Grouped normal/sanitizer checks, one combined guest per build and one
full Qt gate reuse unchanged writer/fault/polkit/customization evidence.
Application binary updates, image download selection, localization, remaining
filesystems and all other unresolved Windows features remain in the active goal.


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


## CP71 — advanced device classes (Preview 40)

Grouped Windows Alt+G/Alt+W/Ctrl+Alt+F behavior with native MMC, recognized
Microsoft/Arsenal/KernSafe/bhyve virtual disks, opt-in VMware and non-USB
removable disks. Virtual disks default enabled; virtual/VMware preferences are
saved, while non-USB listing is session-only and restores the previous HDD
choice. One selected native class is frozen in the strict device protocol and
freshly verified by preflight and the authorized helper, including capture.

Non-USB HintSystem metadata needs native protected-use validation because it
also marks unused secondary disks. Every new class requires the existing kernel
mount/backing and caller-home checks; HintIgnore and source/system/application
protection remain. Unknown fixed internal disks stay excluded. Default card
size filtering and original sandbox/small antivirus virtual-disk exclusions apply.
See [device discovery](linux-device-discovery.md#advanced-device-classes-cp71--preview-40)
for exact native provider scope and remaining loop/NBD, drive-less object and
Microsoft Dev Drive identification work.

Grouped normal/sanitizer qualification passes 115 component cases and four new
private-D-Bus discovery/invalidation cases per build. Each combined guest passes
three ordinary Qt FAT32 jobs on real simulated SCSI classes, kernel filesystem
readback, read-only capture geometry and released claims, one authorization per
job and READY after helper exit. A virtual target containing the caller home is
refused before writes with an empty UDisks mount list and an unchanged whole-disk
hash. The unselected USB baseline remains unchanged. The established writer/
boot and helper-fault evidence is reused; no write semantics changed. One full
nine-suite Qt gate and the two package inspections finish this batch. Exact
results and hashes are recorded in `.rufus-test-artifacts/cp71/checks.json`.

Initial guest attempts corrected fixture disk-name ordering, the caller-home
rootfs backing and absent udev aliases; final native checks were retained. A
post-guest host dispatch initially tried the old baseline MBR reader after all
three class jobs passed; the final route checks the untouched baseline instead.
All disposable media are removed after exporting logs/manifests. No host media
is written or mounted, and no Windows application is executed. Full Windows
parity remains active across the remaining backlog.
