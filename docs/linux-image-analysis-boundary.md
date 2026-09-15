# CP3: image analysis, reuse and Linux adapters

Updated: 2026-09-14. Source baseline:
`18ae93bf37f94764d77216cff4168e0549a14c4d`.

This records the investigation and the first shared-source implementation.
**Media logic mixed with GUI code must be separated in Rufus itself.** The user
accepts those focused upstream changes and favors small native API adapters for
remaining platform dependencies. CP3b extracts post-scan media policy and the
image report; CP3c1 extracts per-file classification. CP3 application integration
is not complete. Prefer [native Linux implementations](linux-native-reuse.md)
of platform work before adapting a Windows implementation of the same operation.
**Wine and Winelib are explicitly excluded.** The analogy concerns small
compatibility functions only; the application and its dependencies remain native
Linux code, using Qt and Linux/POSIX APIs.

## Recommended order of choices

1. **Compile existing source unchanged.** Use its existing platform support and
   a Linux build configuration. This already works for the bundled libcdio
   experiment below; it needs no replacement ISO parser or Qt file abstraction.
2. **Provide an alternate implementation of an existing Rufus interface.** Keep
   callers and signatures where practical. The reporting adapter already does
   this. The original `ms-sys` algorithms and alternate `file.c` are another
   concrete example for a later checkpoint.
3. **Use a narrow Windows-compatible adapter where the mapping is sound.** A
   subset of synchronous file operations can be implemented with POSIX calls.
   Keep these adapters in the Linux directory and select them through CMake.
4. **Separate mixed GUI/media logic into shared C code.** This is required when
   a candidate contains both responsibilities. Both frontends use the same
   media rules; dialogs and controls stay in their frontend. Make the source
   moves small and testable. An API adapter is not a reason to emulate GUI
   dependencies inside media code.

We should optimize total maintenance across Windows and Linux, rather than
source-line reuse alone. Retain upstream names, behavior, reporting conventions
and the existing serialization of process-global state. A new generic engine,
filesystem interface, callback registry or collection of C++ classes is not
needed for this checkpoint.

## What the current scan actually calls

```text
src/rufus.c: ImageScanThread
  -> ExtractISO(image_path, "", TRUE)                  src/iso.c
       -> UDF first; ISO9660/Rock Ridge/Joliet fallback
       -> ScanUdfFiles / ScanIsoFiles                 src/iso_scan.c, read-only
            -> GetIsoFileProps / CheckIsoScanFile      src/image_file.c
       -> Syslinux / InspectEfiImage / ScanWinPeOptions / GRUB / EFI content
       -> WIM metadata checks (Windows host; native integration pending)
  -> IsBootableImage                                  src/vhd.c
       -> compression / virtual-disk handling
       -> AnalyzeMBR                                  src/drive.c
            -> original ms-sys recognition routines
  -> Windows version and Secure Boot information
  -> ApplyIsoPolicy(&img_report)                     src/image_policy.c
  -> Win32 controls, messages and default selections
```

`ExtractISO(..., TRUE)` is an application function, not a standalone ISO reader.
UDF and ISO9660 traversal now call shared scan-only functions. Platform-specific
scan finalization and extraction still have platform dependencies in this file.
Dead-code elimination alone is not a stable interface for excluding them.

The main source boundaries are:

| Area | Source | Reuse treatment |
| --- | --- | --- |
| ISO9660/UDF decoding and image-file I/O | `src/libcdio/` | Compile bundled sources with Linux configuration; existing stdio/iconv support performs the Linux work. |
| Image report and flag meanings | `src/image_report.h`: `RUFUS_IMG_REPORT`, associated constants/macros | Moved from `rufus.h` in CP3b. CP3c7 adds explicit Syslinux presence/detection/inference fields; existing selected-version fields remain for installation callers. |
| Per-file image rules | `src/image_file.c`: `GetIsoFileProps`, `CheckIsoScanFile` | Shared in CP3c1; shared ISO/UDF accounting and the extraction wrapper use these rules. |
| Tree traversal | `src/iso_scan.c`: `ScanIsoFiles`, `ScanUdfFiles`; `src/iso.c`: extraction | ISO9660 separated in CP3c2; CP3e shares UDF traversal and common file/path/block accounting. Windows and Qt use the same scan functions and UDF-first routing. |
| String arrays | `src/strarray.c` | Original utility moved from `stdfn.c`; both platforms use the same storage and ownership rules. |
| File content reads | `src/iso_read.c`: `ReadISOFileToBufferExt`; `src/iso.c`: original wrapper | Shared in CP3c3 using the existing native libcdio calls. The wrapper retains preferences/log settings and the original signature. |
| Syslinux version interpretation | `src/syslinux_version.c`: `GetSyslinuxVersion` | Shared in CP3c4, preserving the signature, numeric encoding and normalized build suffix. |
| Syslinux configuration and version selection | `src/syslinux_scan.c` | Shared in CP3c7, retaining Rufus's configuration/conflict/fallback rules with explicit presence and detection facts. |
| GRUB content interpretation | `src/grub_inspect.c`: `GetGrubVersion`, `GetGrubFs`, `GetGrubFsMask` | Shared in CP3c5 with explicit report/list arguments and original label normalization from `parser.c`. |
| EFI banner diagnostics | `src/efi_boot_info.c`: `GetEfiBootInfo` | Shared in CP3c6; no PE dependency or report mutation. Retains the existing Shim/systemd-boot log messages. |
| Embedded EFI/FAT presence | `src/embedded_efi.c`: `InspectEfiImage`; `src/syslinux/libfat/` | CP3f1/CP3f2 share ISO9660/UDF inspection. Uses the existing FAT reader and storage callback; the Linux build supplies POSIX aligned allocation. UDF cache fills use the bundled seek/block APIs. |
| WinPE options | `src/winpe_scan.c`: `ScanWinPeOptions`, `InspectWinPeOptions` | CP3g1 shares the original priority and `/minint` decision, using original-case SIF paths and size-limited reads. WIM queries remain separate. |
| PE and Secure Boot analysis | `src/parser.c`: PE helpers; `src/hash.c` and `src/rufus.c`: signature/revocation workflow | Separate later boundary, with explicit buffer lengths, native cryptography and dedicated metadata fixtures. Not a dependency of banner reporting. |
| Scan finalization and media policy | `ExtractISO` scan cleanup; `src/image_policy.c`; `ImageScanThread` | Distribution, GRUB filesystem, DD-only and Syslinux selection decisions are shared. CP3c8 shares projected size, label handling and content-stage orchestration; CP3e adds UDF coordination. Platform metadata and final raw/media decisions remain outside the shared operation. |
| Reporting and workflow | `SendMessage`, `PrintInfo`, `PrintStatus`, Win32 controls | Keep direct GUI calls in the frontend. Shared operations report through existing C reporting entry points and return decisions; Qt delivers reporting through the existing job boundary. |
| Host storage and privilege | `dev.c`, `drive.c`, `format.c`, UDisks2/polkit | Use Linux service implementations. Windows device handles and control codes are not the proposed device-management interface. |

Concrete entanglements matter more than the function names:

- `ScanIsoFiles()` and `ScanUdfFiles()` handle traversal without GUI or extraction
  dependencies. The Windows `check_iso_props()` wrapper now only handles
  extraction decisions such as `WimSplitFile()`; its scan bookkeeping was removed.
- `ExtractISO()` sends marquee/progress messages, reads global preferences,
  checks WIM metadata, and selects Syslinux configurations after traversal.
- `ImageScanThread()` now calls shared Red Hat/SUSE, GRUB filesystem and ISOHybrid
  policy after ISO scanning. Its dialogs and control updates remain in place.
- `StrArray` is now shared ordinary C; the Linux build maps its `_strdup` call
  to POSIX `strdup`. Other helpers and `safe_*` macros still need individual
  review because some use Microsoft CRT or platform services.

These are bounded source splits and platform adapter tasks. They do not require
a wholesale redesign of Rufus's globals or a new object model.

## Small Windows-to-Linux compatibility layers

Use Windows-compatible function signatures where that keeps selected Rufus
callers unchanged, and implement the required behavior directly through native
Linux/POSIX APIs. This involves no Wine runtime, Winelib, winegcc toolchain or
Windows helper executable. Wine was mentioned only as an analogy for API
translation, not as a dependency or implementation option.

| Option | Advantage | Cost for this port |
| --- | --- | --- |
| Existing Rufus entry points with Linux implementations | Preserves callers and keeps Linux code separate; already used for reporting. | We maintain the small number of alternate implementations. |
| A limited Win32 compatibility layer | Can allow larger original C files to compile unchanged. | Must implement real semantics and enough declarations to compile all included code, including branches not used during scanning. |
| Targeted shared-source moves | Avoids carrying unrelated Windows dependencies into Linux. | Requires small upstream-visible edits and Windows regression evidence. |

Separate GUI/media responsibilities first, then retain existing Rufus
interfaces and add small API adapters where they help. Compiling all of `iso.c`
through one bridge is not the objective: its scan and extraction responsibilities
also need a deliberate boundary.

### Adapter contracts worth preserving

| Existing interface | Plausible Linux implementation / constraint |
| --- | --- |
| `uprintf`, `UpdateProgress`, related reporting | Existing C-linkage Qt adapter; queued delivery to the UI. |
| `fopenU`, file metadata helpers | Native file calls with explicit UTF-8/path and size contracts. |
| `CreateFile` / `ReadFile` / `SetFilePointerEx` / `GetFileSizeEx` / `CloseHandle` | A documented synchronous regular-file subset, using a consistent handle representation, 64-bit offsets, short-read and error handling. Add only what the selected path uses. |
| `GetLastError`-style status | Preserve the error meanings needed by callers. An arbitrary `errno` cast is insufficient. |
| Direct Win32 progress/control messages during analysis | Move these to the Windows caller or replace them with an existing semantic reporting operation at the shared boundary. Do not implement message dispatch or fake widget handles on Linux. |
| `ms-sys` `read_data` / `contains_data` | Alternate Linux file implementation with its existing contract; preserve the original algorithms. Keep its `FILE*`/`FAKE_FD` convention consistent across callers. |

Unsupported calls must fail explicitly and tests must detect them. A stub that
reports success for an operation it did not perform would undermine the value
of source reuse. Do not map synchronous Windows return-value behavior blindly
onto queued Qt delivery. Path encodings, calling conventions, fixed-width types
and handle ownership must agree at each boundary.

Full `DeviceIoControl`, COM, registry, virtual-disk mounting and Win32 GUI
compatibility are separate, much larger commitments. The existence of a small
file bridge does not justify expanding it into those areas.

## Evidence: bundled versus system libcdio

The initial standalone [probe](../linux/probes/libcdio/README.md) compiled
**20 original C translation units unchanged** with a Linux/glibc configuration,
then linked and ran a regular-file reader. The source's original stdio and
iconv implementations provide Linux I/O and conversion. The two optical-device
entry points are disabled as they already are in Rufus's `iso.c`.

Compared the bundled `2.2.1 (Rufus)` source against the installed system
`libiso9660`/`libudf` **2.4.0**, using matching headers and API configuration:

| Probe | Bundled | Installed system |
| --- | --- | --- |
| Read ISO volume label and a 4096-byte file through a Unicode image path | Expected label, size and first 16 payload bytes | Same |
| Read `/[BOOT]/0-Boot-NoEmul.img` from the same El Torito fixture | Expected payload prefix found | Entry missing |
| Empty file, plain text, ISO truncated before its volume descriptors | Rejected | Rejected |

Rufus's bundled reader implements synthetic `[BOOT]` entries, and its
`HasEfiImgBootLoaders()` and related logic use those paths. Local history records
further changes in `c81f3cfb` and `e8207ea9`. The measured difference supports
retaining this bundled reader initially; it is not a claim that system libcdio
cannot read El Torito information through any other interface.

There is also an ABI detail: `DO_NOT_WANT_COMPATIBILITY` changes the layout of
`iso9660_stat_t`. The bundled build and all its consumers must agree on it.
The installed system library uses the legacy layout. Applying Rufus's define
to that system consumer gave an incorrect size in the initial probe; the final
comparison uses each library's matching layout. Never mix bundled headers with
a system binary or propagate this define globally.

Limits of that initial experiment: Linux x86-64/glibc only, with synthetic
non-executable payloads. Existing bundled warnings include signedness,
fallthrough and a format mismatch. Later checkpoints below add reader fixes,
feature-detected builds, RR/Joliet tree fixtures and sanitizers. Valid UDF
traversal, EFI/FAT inspection and actual bootability remain separate work.
Windows behavior will be checked later in a real Windows environment.

## What should actually be common

The shared behavior should eventually include image classification, bootloader
recognition, report semantics, distribution exceptions, supported media modes,
configuration transformations and boot-sector algorithms. Host file/device
access, authorization and widgets get their platform implementations.

For CP3, preserve these contracts in particular:

- `has_4GB_file` is a count/bitfield with special WIM/ESD/SWM meanings, not a
  boolean. `has_efi` and `has_grub2` are also masks, not simple yes/no values.
- `is_bootable_img` has error/forced-image distinctions. A boot signature is a
  recognition heuristic, not evidence that the generated medium boots.
- Upstream can clear `is_iso` when policy requires DD mode. It does not always
  mean only “an ISO filesystem was detected”.
- Fields not analyzed by the first Linux path must remain explicitly unknown
  at the application boundary. A zeroed partial report must not authorize media
  creation or silently disable a required workaround.
- Keep source paths, actual image size, projected extraction size and internal
  image paths distinct. Preserve case where Rufus retains it for later reads.

An ordinary concrete C scan state can be introduced if a particular function
needs explicit inputs. Windows can pass its existing state; the Qt worker can
own its state and copy the completed report. Reentrancy of the entire Rufus
engine is not a CP3 requirement; the current serialized job boundary remains.

## CP3b: first shared policy split

`src/image_report.h` now owns the existing report, associated types and neutral
flag macros. `rufus.h` includes it and retains macros that depend on host
settings or filesystem selection. Field order, sizes and numeric meanings are
preserved; Windows `BOOLEAN` fields become `uint8_t`, with the same byte width.
There is no Linux copy of the report.

`ApplyIsoPolicy(RUFUS_IMG_REPORT*)` in `src/image_policy.c` uses the original
`cregex` implementation and label patterns. It sets `rh8_derivative` and
`disable_iso`, and returns whether the image requires DD mode. It takes a
complete report and has no GUI, device, settings or global-report dependency.
It retains the existing `uprintf` entry point for internal regex diagnostics.

The Windows `ImageScanThread` calls it at the original point after bootloader
inspection and property logging. Notification text, notification timing and
the subsequent `img_report.is_iso = FALSE` remain in the Windows caller. Linux
can use the same decision from its future scan job. No GUI callbacks or new
platform conditionals are introduced into the shared implementation.

Windows Visual Studio and MinGW source lists include the new module and headers.
Portable tests live in `tests/image_policy/`, also included by Linux CTest.
All 199 cases pass against both the extracted module and a temporary harness
of the original policy block at the baseline revision. Report size, alignment
and all 48 field sizes/offsets match in native Linux and freestanding Clang
Windows x86/x64 compile checks. These checks do not replace a Windows build/run.
The normal and sanitizer Linux suites pass all five CTest entries.

Native Windows component CI is prepared for x86/x64 but has not run. Full VS
and MinGW builds plus a Windows GUI behavior check remain required before this
shared change is ready to propose upstream. Nothing has been committed or pushed.

## CP3c1: per-file classification and ISO fixtures

`GetIsoFileProps` returns a concrete `ISO_FILE_PROPS`: config and old-module
flags plus whether the file belongs in config/isolinux lists or is `ldlinux.c32`.
It computes all file facts in both modes. Previously scan mode did not populate
the unused generic `.cfg`, GRUB-config and menu-config flags; the new contract
deliberately makes properties independent of the operation.

`CheckIsoScanFile` updates the caller's report using the original file rules.
EFI path handling and Windows-file rules are private named helpers. No dynamic
allocation, GUI access, device access, global report or callback is needed.
Shared filename tables remain defined once and are also used by Windows
extraction/bootloader inspection. Report fields and masks are unchanged.

The Windows `check_iso_props` wrapper keeps config and isolinux path lists,
`has_ldlinux_c32`, block totals, skip decisions and WIM splitting. Its callers
and extraction workflow remain in place. The two CRT string comparisons use
native Windows functions or a CMake-selected POSIX mapping on Linux. Bounded
path copies retain Rufus's original truncation behavior.

Validation: 152 component checks pass. A temporary harness comparing the old
and new Windows wrappers passes 44,280 combinations, checking report bytes,
path-list counts, block totals, skip results and WIM split arguments. The only
normalized properties are the three previously unused scan-mode flags above.
WIM operations were recorded by the harness, not executed.

The separate bundled-reader fixture passes 17 entries in two generated Rock
Ridge ISOs, including original filename case, size-based rules and a real
symlink. It uses explicit known paths, not a replacement production walker.
Normal and sanitizer runs pass. The sanitizer run found two pre-existing
reader issues, fixed narrowly in `iso9660_fs.c` (absent symlink copying) and
`cdio/bytesex.h` (unsigned shift). Existing bundled compiler warnings remain.

The six main CTest entries also pass normally and under address, undefined-
behavior and leak checks. Linux CI includes the fixture step; native Windows
component CI covers both shared modules on x86/x64. CI and full Windows GUI
validation have not run. This completes CP3c1 locally, not full scan integration.

## CP3c2: shared ISO9660 tree traversal

`ScanIsoFiles` in `src/iso_scan.c` is the scan-only ISO9660 walker used by the
Windows caller and the Linux test driver. Its concrete state holds the caller's
report, original `StrArray` config/isolinux lists, block counters and name
options. It preserves the traversal order and Rufus's RR/Joliet, firmware
symlink, Mint NTFS and deep-directory behavior. Direct progress setup stays in
the frontend caller. `IsIsoScanCancelled()` is a synchronous host entry point;
there is no GUI callback object or platform conditional in shared traversal.

`StrArray` moved into `src/strarray.c` with native CRT/POSIX selection at build
time. Failed growth retains the old allocation and capacity for cleanup/retry.
The scanner checks path-list insertion errors and rejects overlong tree paths
instead of truncating them. These are deliberate error-handling changes, with
fixture coverage. The scan no longer allocates the extraction function's I/O
buffer for every directory.

`linux/cdio/` now defines the bundled native reader once for both the Linux
build and the comparison probe, using CMake feature/byte-order/offset checks.
All eight main Linux CTest suites pass normally and under address, undefined-
behavior and leak checks. Generated trees cover both name extensions, symlink
accounting, dynamic lists, cancellation and partial/error results. The previous
known-path fixture and bundled/system comparison still pass. See the
[checkpoint record](testing-and-checkpoints.md#cp3c2--shared-read-only-iso-tree-scan-2026-09-13)
for commands and exact limits.

This is tree metadata, not complete image analysis. The caller must discard
findings on error/cancellation and explicitly mark deep-directory short circuits
as partial. Bootloader content, scan finalization, UDF and GUI job integration
remain to be moved/tested. Windows validation is deferred by the user's request
and does not gate Linux progress. The form and SELECT behavior are unchanged.

## CP3c3: shared image-file buffer reading

`ReadISOFileToBufferExt` in `src/iso_read.c` moves the existing buffer reader
behind an explicit ISO extension-mask argument. Private ISO9660 and UDF helpers
keep resource cleanup with each format's operations. UDF is still tried first,
and a successful UDF open prevents fallback on a missing root/file. Name options,
the inclusive 1 GiB limit, bulk reads, byte count, trailing NUL and caller `free`
contract remain intact. Existing Windows callers retain the original three-
argument function; its wrapper supplies preferences and adjusts logging.

Deliberate error-handling changes: reject directories, validate null arguments,
check unsigned file lengths before narrowing, and return empty files without an
allocation. Zero/null still means empty or failed, matching the old API. The
shared reader does not change application log preferences or introduce GUI
callbacks. It remains synchronous, with no cancellation inside a library read.

All ten Linux CTest suites pass normally and under address, undefined-behavior
and leak checks. Generated ISO tests compare complete binary contents and name
options, including a synthetic El Torito path and ordinary truncated file data.
API-level tests cover UDF preference and cleanup, short reads and allocation/size
errors without creating large files. These tests do not establish real UDF disk
parsing; that remains a separate fixture requirement. No new tool dependency,
media write, UI behavior or native Windows validation was introduced.

## CP3c4: shared Syslinux version detection

`GetSyslinuxVersion` and the version macros are now in a neutral C module/header.
The existing installer stays in `syslinux.c`; it is not a dependency of Linux
recognition. Private functions separate number parsing and suffix normalization.
No Linux API adapter is necessary. Both Windows callers retain the same public
function. The image caller supplies `size + 1` to include its reader-added NUL;
embedded resources retain their actual lengths and internal banner termination.

Preserved behavior includes the first usable SYS/ISO-prefixed marker, search
margins/minimum span, major/minor encoding, duplicate-version removal, trailing
spaces/stars and the existing suffix character substitutions. The bounded API
now requires termination within the supplied span, validates decimal fields,
rejects out-of-range versions without assertions and preserves a bare version's
terminator. Original numeric whitespace/plus handling through `strtoul` is
retained. The suffix still borrows the writable input buffer. Reusing a known
terminator and avoiding repeated suffix-length scans keeps scanning linear.

The 46-case component suite includes both bundled payloads. A generated ISO
exercises the shared content reader plus parser on those payloads and synthetic
banners, including a terminator supplied at EOF. The original parser in a
temporary native Linux harness produces the same fixture results. New boundary/
invalid-input behavior is covered by the shared-parser tests, not that comparison.
All twelve Linux suites pass normally and with address/undefined-behavior/leak
checks. Full Windows validation remains deferred.

The agreed installation policy keeps detection and selection separate: reuse
Rufus's embedded-v4/v6 fallback when needed, recording it as inferred, then
select a native implementation that can install the chosen payload. Preserve
Syslinux presence even when its detected version is unknown; do not substitute
the host tool's version as an image fact. The [plan](linux-port-plan.md#syslinux-detection-and-installation-selection--agreed-2026-09-13)
records the scan-finalization audit and policy/image/boot tests. The Windows
fallback and conflicting-loader policy are unchanged; CP3c4 did not implement
Linux selection or installation.

## CP3c5: shared GRUB inspection

The original GRUB version/filesystem functions now live in `grub_inspect.c`,
with the report and filesystem array supplied by the caller. There is no global
image state, GUI API, file I/O or callback framework in this module. Windows
keeps traversal, file selection and formatted filesystem logging in `iso.c`.
Linux uses the same C source and existing reporting endpoint.

Private version reading retains Rufus's banner spellings, CentOS skip and
32-byte search margin. It finds the last usable version in each buffer, while
the public function preserves a previously populated report version. Rufus's
zero-version exclusion, nonstandard-prefix flag, debug-symbol suffix and exact
2.06 label suffix stay shared. The sanitizer from `parser.c` was moved unchanged
apart from private linkage and its array-length expression; its existing
transformation order is retained. The report's version remains Rufus's payload
lookup string, including those compatibility suffixes.

Bounds are explicit: version strings must terminate in the accessible span,
long versions retain the 191-byte output limit, and overlapping candidates reuse
a known terminator. Filesystem names remain nonempty and shorter than 12 bytes;
the bounded lookup copies them into the existing string array. Case-sensitive
uniqueness/order and case-insensitive FAT/exFAT/NTFS mapping are preserved.
Allocation failure returns false with partial names retained; the Windows
caller logs that failure. Content callers include the reader's extra NUL in the
supplied span. Consequently the search margin is measured against that accessible
span, including its added byte.

All fourteen Linux tests pass normally and with address/undefined-behavior/leak
checks. There are 36 portable component cases and two additional Linux
allocation-failure/retry cases. An eleven-file generated ISO covers combined
content reads, cross-file selection and filesystem accumulation. The original
functions passed the same image expectations in a temporary Linux harness;
new bounded/failure tests run only against shared code. These are synthetic
marker fixtures, not real EFI/GRUB binaries or a boot test. Native Windows
validation is still deferred. See the [test instructions](../tests/grub_inspect/README.md).

## CP3c6: shared EFI banner diagnostics

Source inspection corrected the planned boundary: `GetEfiBootInfo` only searches
for Shim/systemd-boot version banners and logs one diagnostic. It does not call
`GetPeSection` or change `img_report`. PE-section lookup is used by
`IsRevokedBySbat` in `hash.c`, under the separate `GetBootladerInfo` Secure Boot
workflow. PE/signature/revocation coverage must remain explicit later work;
moving it is unnecessary for the existing banner behavior.

`efi_boot_info.c/.h` now supplies the same entry point to both platforms, with a
const input buffer. The function retains first-match and marker ordering,
64-byte search margins, whitespace tokenization, 32-byte truncation and existing
log text. Bounded printf precision replaces the original write of a NUL into
the buffer; casting to unsigned byte before ctype handles byte data portably.
The Windows call still passes the actual file size, with no additional byte.
No new adapter, allocation, callback or report model is introduced.

The 34 component cases check exact messages and unchanged inputs. Nine
generated-ISO fixtures exercise the shared reader and banner routine. Two
installed systemd-boot binaries (x64/IA32, version `261.3-1-arch`) also passed
through that image path; their hashes and reproduction command are in the
[test instructions](../tests/efi_boot_info/README.md). They were read only as
data and were not added to the repository. A temporary original-routine harness
produces the same synthetic/real-file diagnostics. These results cover banner
recognition, not PE validity, trust, or boot behavior; no real Shim was tested.

## CP3c7: shared Syslinux scan policy

`syslinux_scan.c` now holds three ordinary C operations: choose a configuration,
consider a successfully read loader candidate, and select a detected/fallback
version. Reports and source paths are explicit arguments. Candidate source
paths are borrowed only until selection reporting finishes; config and suffix
strings are copied. The functions allocate nothing and perform no I/O. Windows
retains file reading in its existing `iso.c` loop and uses these shared rules.

Four fields appended to the report make the agreed distinction explicit:
`has_syslinux`, `sl_detected_version`, `sl_detected_version_ext` and
`sl_version_inferred`. The existing `sl_version`/string/ext fields remain the
installation choice. `HAS_SYSLINUX` now uses configuration presence. The source
audit found the image version assignments in this scan block; Windows consumers
continue to receive its selected version. Two added image-policy cases verify
that unknown presence survives and a stale selected version cannot create
presence on its own. This is identification, not a claim of successful boot or
native installer support.

The shared rules retain shortest-path/latest-tie preference, openSUSE override,
case handling, original conflict diagnostics, family warnings and v4/v6 numeric
fallback selection from `ldlinux.c32`. The fallback retains its empty suffix and
is marked inferred without changing detection. A zero supplied fallback slot
leaves the installation version unavailable while preserving presence.

Existing conflict quirks are deliberately retained: root configs compare a
zero-length prefix, and other config-directory prefixes are not required to end
on a candidate directory boundary. An unknown candidate can replace a known one
under that preference; an unreadable file is skipped. The suffix retains its
existing 31-byte report limit. These choices are covered by tests rather than
silently changed during extraction.

One error contract changes: when all configs exceed the report's existing
127-byte path capacity, selection returns false with presence recorded and an
empty path. The Windows caller sets a scan error, and Linux's probe fails. This
replaces the original underscore placeholder and does not implement longer
extraction paths.

All eighteen Linux suites pass normally and with address/undefined-behavior/leak
checks. There are 29 standalone policy cases, two added image-policy cases, and
17 generated images using the shared tree scanner, content reader, version
parser and policy together. Sixteen supported-path images also pass against the
original selection block in a temporary Linux harness. New unavailable-fallback
and overlong-only config behavior is tested only against shared code. The
[test instructions](../tests/syslinux_scan/README.md) record scope and contracts.
Native installation, Qt integration and Windows execution remain separate work.

## CP3c8: shared ISO9660 finalization and coordinator

`src/iso_analyze.c` adds `AnalyzeIso9660` and the extracted `FinalizeIsoScan`.
`src/iso_bootloaders.c` moves the remaining Syslinux read/selection loop and
BIOS GRUB/EFI content loops out of `ExtractISO`. Windows calls those same
functions around its existing embedded-FAT, WinPE and WIM checks. It retains
its content-name options and UDF preference; its GUI/extraction flow stays in
`iso.c`. String arrays for GRUB filesystem names are now owned by that content
stage instead of a global. No Qt types, function pointers or new GUI bridge
enter these sources.

The ISO9660 coordinator takes a path and known regular-file size, preserving the
host's responsibility for file selection/stat. It opens and closes readers,
creates/destroys path arrays, fills the existing report and returns owned plain
data. The initial scan uses Rufus's Rock Ridge preference; the content stage uses
that same view. A recognized UDF container returns an explicit pending result
before scanning an ISO tree, because the shared buffer reader prefers UDF.
This conservative boundary also covers UDF containers with unavailable roots;
only Windows currently has the traversal needed to decide its fallback.

| Result | Meaning |
| --- | --- |
| Complete | Supported ISO9660 metadata stages finished. Does not authorize installation or prove bootability. |
| Partial | Report includes pending reasons: UDF, deep directories, unavailable content, embedded EFI/FAT, Windows metadata, Secure Boot or a claimed volume larger than the file. |
| Cancelled | Host cancellation observed between operations; handles/buffers freed and result cleared. |
| Failed | Invalid input, failed tree/header read, unsupported config path or path/filesystem storage failure; result cleared. |

Unknown version banners remain successful observations; inferred Syslinux
installation versions do not become image facts. The existing content reader
reports empty files, allocation failures, unsupported sizes and I/O failures
through the same zero return, so these produce an unavailable-content reason.
Deep-directory reports have no completed-tree flag, projected size or selected
bootloader version. Separate raw/hybrid detection and final media policy are
required later; the coordinator deliberately does not call `ApplyIsoPolicy`
with absent raw-image facts. Secure Boot pending is not an unsigned verdict.

`FinalizeIsoScan` retains the original label's first-character trimming quirk,
float `1.01f` size allowance, separate firmware extra blocks and signed mismatch.
Unrepresentable size arithmetic now fails explicitly. The content stage also
reports GRUB name-storage failure instead of allowing incomplete filesystem
capabilities to look complete. Windows retains its former optional-read policy;
the new coordinator distinguishes partial content from complete metadata.

All twenty Linux suites pass normally and under address/undefined-behavior/leak
checks. Twenty-two generated image/plain-file fixtures exercise the complete
operation, including failure/cancellation injection; 48 component checks cover
arithmetic, reset and UDF dispatch contracts. Nine positive fixtures produce
identical reports and logs using the pre-split content loops in a temporary
Linux harness. This does not validate WinPE/WIM/embedded EFI, actual UDF decoding,
native Windows execution or booting. Source manifests include the new modules.

Next is CP3d: connect supported ISO9660 metadata and explicit partial results to
SELECT with the existing concrete job and Widgets status/log areas. Keep START
disabled and test cancellation, selection changes and stale-result rejection.

## CP3d: Qt integration of the shared metadata operation

`ImageAnalysisJob` derives from the existing `RufusJob`, calls `AnalyzeIso9660`
and publishes a copied C analysis record plus status/problem text. Shared
analysis headers expose plain report/options/status types without requiring
libcdio headers in the Widgets/controller code. Qt stays out of shared C; the
Windows runtime and form are untouched by this integration.

The controller owns one reusable thread. Selection request numbers accompany
queued logs/results, so completed but undelivered work cannot replace a newer
selection. A replacement waits for the previous thread to finish and only the
latest selection starts. Failure and cancellation discard report fields. Generic
job exceptions and process-guard rejection produce explicit failed results.
Thread interruption is queried directly at shared C scan boundaries.

The original form supplies the displayed label, marquee progress, CLOSE/CANCEL
button and status/log areas. Device/image/advanced selection controls are
unavailable during analysis; the log stays usable. Main-window close during a
scan cancels and keeps the window open, as in Win32. Final teardown joins the
worker; an individual library read remains synchronous and can delay shutdown.
There is no production test hook or worker factory.

The two existing Syslinux payloads are packaged as Qt resource data, and the
shared parser obtains fallback versions from their bytes. No host installer is
queried, and inferred choices never overwrite detected image facts. File type,
size and modification time are checked before/after analysis; write-time identity
validation and changes after completion remain outside this read-only checkpoint.

All 21 Linux suites pass normally and under address/undefined-behavior/leak
checks. Fifteen new Qt functional cases use real generated ISO9660 images and
link-time race gates, covering queued ownership, stale results/logs,
cancellation and restart, file mutation, exception/overlap recovery and teardown.
The analyzed-image screenshot was inspected against the existing layout. These
checks do not qualify native Windows rendering, all desktop dialogs, actual UDF
traversal or real distribution boot behavior.

## CP3e: shared UDF traversal and real image coverage

`ScanUdfFiles` lives beside `ScanIsoFiles`, using the same small file-accounting
function and original image rules. It consumes its directory iterator, skips
parent entries, preserves case and UDF symlink flags, records config/loader paths
and rounds file sizes to 2048-byte blocks. It has no extraction buffer, Windows
calls, GUI handles or callback registry. Both host callers use it; Windows keeps
its extraction code. No new shared source registrations are needed.

The coordinator is now `AnalyzeIsoImage`, with an explicit filesystem value in
its plain result. UDF recognition no longer yields an unimplemented-stage marker.
Tree and content reads prefer UDF. After a successful UDF open, a missing root or
unreadable directory is a failed scan, with the result cleared. This deliberately
replaces the old Windows scan's root-failure ISO fallback and silent skipping of
unreadable child directories. ISO fallback still occurs after failed UDF open;
Windows extraction retains its previous fallback. UDF scan paths now share the
260-byte limit instead of allocating unbounded recursive paths.

The optional ISO PVD supplies size checks only, as before. Without it,
`volume_size_known` is false; zero mismatch must not be treated as a verified
size. Shared bootloader inspection, unknown-version handling, partial reasons and
Qt cancellation/result ownership remain unchanged. The form is unchanged.

The bundled reader now rejects partial sector reads, checks directory storage
and record bounds, propagates allocation failures, uses little-endian directory
fields and converts compressed 8-bit UDF Unicode to UTF-8. The walker distinguishes
EOF from a failed read using the existing iterator's remaining bytes, without
adding another callback or reader API.

Thirteen generated UDF fixtures pass with genisoimage 1.1.11 and cdrtools mkisofs
3.02a09. Seventeen reader checks cover short reads, bounds and allocation cleanup;
49 coordinator checks include failure after UDF recognition. Two additional Qt
cases exercise UDF reports, pending Windows metadata, cancellation and restart.
All 22 Linux suites pass normally and with address/undefined-behavior/leak checks.
This qualifies these generated layouts, not every UDF revision/allocation mode,
real distribution image, native Windows execution or boot behavior.

## CP3f1: shared ISO9660 embedded EFI/FAT inspection

`InspectIsoEfiImage` separates the read-only search from `iso.c`. It takes a
borrowed ISO handle, a candidate path and a presence output, with no report or
GUI globals. Windows keeps its original `HasEfiImgBootLoaders` signature as a
wrapper. The Linux coordinator calls the same inspection; its result sets the
existing `has_efi = 0x8000` marker only after completed detection. Successful
absence clears embedded-EFI pending; unreadable candidates retain it. Presence
leaves Secure Boot pending. Cancellation and errors discard analysis results.

Reuse stays concrete: libfat's existing storage callback performs bounded reads
with the original 32 KiB read-ahead. The first active MBR FAT partition and its
ISO-alignment rule are preserved, with partition/FAT size bounds. No whole-image
buffer, host mount, Qt callback registry or generic filesystem interface is added.
The library's process-global sector size is saved/restored around inspection;
callers retain the original serialization requirement. Linux selects a small
`posix_memalign` adapter at build time.

The search preserves Rufus's architecture names and `~1` aliases for the two
long boot names. The aliases remain heuristic. It now combines both FAT32 cluster
halves for directories and rejects empty files/directory entries as bootloaders.
Supporting reader changes clean up failed opens, use portable bytewise endian
helpers, bound directory cache storage and fail on directory-chain cycles or
invalid successor clusters. These targeted corrections are shared with Windows;
they do not constitute a full FAT parser audit. Windows extraction and its
original FAT callback remain separate.

The original embedded reader was ISO9660-only, so **CP3f2** will add UDF access
as its own tested transport boundary. Linux leaves UDF candidates pending and
does not inspect the companion ISO view. The legacy Windows caller can still
supply that companion view; its caller/view behavior has not been changed here.
El Torito boot load counts also need careful qualification: they may expose
only the initial load, not the full FAT extent. A header's claimed size cannot
by itself authorize reads beyond the known candidate range.

Twenty-seven generated container fixtures cover FAT12/16/32, 4 KiB sectors,
all eight architecture names, FAT32 high directory clusters, EOF read-ahead,
absence, partition bounds, truncation and UDF deferral. Fault injection checks
read/storage failure, cancellation and restored state. A new Qt case checks
found/absent/unreadable outcomes and cancellation through the unchanged form.
All 23 suites pass normally and with ASan/UBSan/leak checks; Qt analysis now has
18 behavioral cases. These are metadata tests, not PE/signature validation or
boot tests. Native Windows and remote CI remain unexecuted.

## CP3f2: bounded UDF access to embedded FAT

The shared entry point is now `InspectEfiImage`, accepting exactly one selected
ISO9660/UDF handle. It never switches filesystem views. `AnalyzeIsoImage` and the
Windows wrapper now pass UDF when available, replacing Windows's former embedded
inspection through the companion ISO. The caller owns/serializes the handle;
UDF file position is changed by inspection. Root/file entries are owned locally
and freed on every outcome. FAT sector-size settings are still restored.

The new storage path uses existing `udf_setpos`/`udf_read_block`, filling the same
32 KiB cache across extent boundaries with explicit seeks. FAT rules, architecture
names, presence results and GUI reporting are unchanged. This adds no generic
reader abstraction or callback registry. Windows extraction remains separate.

The bundled offset translator needed a focused correction: a seek inside an
extent previously reported the whole extent's length as readable. It now returns
the remaining bytes, checks descriptor bounds and reads little-endian fields.
Block reads stop at extent/file boundaries and check representable positions.
Recorded short/long descriptors in the file entry and the first partition map
are supported. Sparse, indirect/extended and in-ICB layouts remain unsupported.
These fixes do not constitute a full UDF parser audit, nor do they make the older
single-read whole-file buffer operation assemble multiple extents.

Sixteen UDF containers and 22 direct range checks cover three non-contiguous
extents, short/long descriptors, EOF padding, distinct filesystem views and
missing UDF paths without ISO fallback. Failure/cancellation tests check pending
results, cleanup and restored state; descriptor-bound checks run only on the
corrected reader. Qt's embedded-EFI case runs on both UDF and ISO9660, for 19
analysis cases/data rows. All 24 suites pass normally and under ASan/UBSan/leak
checks. The form and disabled START are unchanged. Native Windows, remote CI,
Secure Boot and real boot behavior remain unqualified.

## Bounded checkpoints

1. **CP3a — This investigation.** Record the source map and measured library
   behavior. Discuss adapter scope before implementing a bridge or
   moving policy functions. This stage is complete as an investigation.
2. **CP3b — Extract post-scan policy and the neutral report.** Implemented and
   verified locally above; native Windows validation remains pending.
3. **CP3c — Isolate and test one read-only ISO path.** CP3c1 classification,
   CP3c2 ISO9660 traversal, CP3c3 buffer reading, CP3c4 Syslinux detection,
   CP3c5 GRUB inspection, CP3c6 EFI banner diagnostics, CP3c7 Syslinux selection
   and CP3c8 finalization/coordinator are implemented above. The ISO9660 metadata
   operation reaches Widgets through CP3d; its partial reasons expose pending embedded
   EFI/FAT, Windows metadata and Secure Boot stages.
   CP3e adds real UDF read/traversal fixtures. Retain Windows callers for later
   native validation.
4. **CP3d — Connect the result to the existing Widgets workflow.** Implemented:
   a concrete `ImageAnalysisJob`, explicit result states, request-tagged queued
   delivery, thread interruption and the original status/log/label controls.
   Fifteen Qt functional cases pass; START stays disabled.
5. **CP3e — Share UDF traversal.** Implemented and verified above, including
   UDF-first routing, cleanup and partial/failure results through the same Qt job.
6. **CP3f1 — Share ISO9660 embedded EFI/FAT inspection.** Implemented and verified
   above using the original FAT reader and generated image files. Presence,
   absence and incomplete inspection remain distinct; Secure Boot stays separate.
7. **CP3f2 — Add UDF access to embedded FAT.** Implemented and verified above,
   with bounded reads, fragmented extents and explicit filesystem selection.
   Qualify incomplete El Torito ranges separately instead of extending them from
   a claimed FAT size.
8. **CP3g1 — Share WinPE options and map Windows metadata.** Implemented:
   `winpe_scan.c`, original-case SIF paths, bounded reads, known/unavailable
   results and Qt integration. All 26 suites pass normally and with sanitizers.
   See the [metadata source map and contract](linux-windows-image-metadata.md).
9. **CP3g2 — Native WIM metadata on ordinary files.** Implemented: native wimlib,
   shared archive information and version normalization, explicit field validity,
   cancellation/ownership checks and generated WIM fixtures. Windows retains
   its path/XML handling and calls the shared archive query/normalization.
10. **CP3g3a — Shared ISO/UDF file ranges.** Implemented: `iso_file.c` separates
    the existing EFI reader into a concrete open/read/close operation, with
    fixed caching, arbitrary byte ranges and selected-view semantics. Generated
    WIMs test exact bytes across block/cache boundaries and fragmented UDF extents.
11. **CP3g3b — Native wimlib transport prototype.** Next, retain Rufus's input
    extension in a native build and use the shared reader. Public system wimlib
    has no arbitrary-reader input hook. Compare native build/source delta and
    embedded metadata with ordinary-file results before choosing the dependency
    arrangement. CP3g3c then connects it to SELECT. Avoid whole-WIM copies.

The shared UDF/ISO9660 image-analysis path now reaches the application with
functional tests. This is a scoped read-only implementation, not full format or
real distribution qualification.
Container WIM/version queries, Secure Boot validation, compressed/virtual images and full
ISO extraction should receive later bounded increments where not required by
the first supported case.
