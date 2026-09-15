# Linux port: tests and development checkpoints

Updated: 2026-09-15

This is the working checklist for the [Linux port plan](linux-port-plan.md).
Physical USB drives are currently unavailable. Development must still produce
observable, tested behavior at each step, with explicit limits on what the
simulation proves.

## Working rules

1. Select one checkpoint and state its behavior, affected area and pass criteria
   before editing. Split a checkpoint if investigation reveals another substantial
   dependency; do not silently expand the work into the next phase.
2. Add the relevant tests with the feature. Test observable results and failure
   handling through actual integration boundaries. Avoid tests that only repeat
   implementation details or confirm a dependency is installed.
3. Build and run the affected tests, fixing failures before continuing. Preserve
   earlier regression coverage. Linux builds and test registration use CMake;
   let Ninja choose its default parallelism. Keep the Windows application on
   Rufus's existing build systems and update their shared-source manifests.
4. Review the diff for scope, the agreed coding style and Windows-source impact.
   Keep each checkpoint suitable for a small independent commit. Do not bundle
   unrelated changes or generate an entire future architecture in advance.
5. Record the result, test command, limitations and next checkpoint here. End
   the work increment at that checkpoint with a concise report. On resuming,
   read this status and inspect the worktree before selecting the next step.

Tests must have bounded waits and deterministic inputs. Use signals, process
completion and explicit conditions instead of arbitrary sleeps. A timeout is a
failure. Missing optional VM dependencies are reported as unavailable/skipped,
never as passing boot validation; a required CI job must fail if its prerequisites
are absent. Preserve useful logs and fixture identifiers for failures.

Windows validation will happen later on real Windows, per the user's decision.
Continue Linux implementation and tests now. Keep shared-source registrations
consistent and record changes for that later validation; Windows execution is
not a gate for continuing these Linux checkpoints.

## Testing without USB hardware

| Level | How it runs | What it establishes | What it does not establish |
| --- | --- | --- | --- |
| Component integration | QtTest, real C caller, worker threads and offscreen Widgets. | Report ownership, thread delivery, progress semantics and lifecycle. | Application workflow or storage behavior. |
| Widgets workflow | QtTest mouse/keyboard actions against the actual window/controller, with synthetic device records. | Selection, enabled controls, state changes, logs and error presentation. | Discovery or kernel device handling. |
| Service integration | Real Qt D-Bus client connected to a private test bus with controlled UDisks2 objects/replies/signals. | Discovery, hotplug handling, authorization responses and request arguments. | Real permissions, formatting, native prompts or driver behavior. |
| File integration | Production analysis/write functions operating on small fixtures and disposable regular files. | Image reports, exact output bytes, file layout and supported I/O failure/cancellation behavior. | USB identity, block-device permissions or bootability. |
| Virtual USB integration | Run the app inside a Linux VM with UDisks2/polkit and a file-backed emulated USB disk. | Guest USB discovery, real storage-service operations, hotplug and authorization integration. | Physical controllers, flash failures or power-loss durability. |
| Boot validation | Boot generated media in a separate VM using the relevant BIOS or UEFI firmware. | The tested image reaches a specific expected boot stage. | Every machine, Secure Boot unless explicitly configured/tested, or completion of an OS installation. |

### Widgets and private-bus tests

Start with a few concrete records: no devices, a removable target, an internal
disk, and two devices with the same display name but different identities.
Keep these fixtures in tests. They must not become a fake production backend
or cause a regular file to be silently treated as a physical device.

For discovery, pass the concrete D-Bus connection to the Linux client. Tests
must explicitly connect to their private bus and fail if it is unavailable;
never fall back to the host system bus. CP2 uses `dbus-daemon` with a small,
separate Qt service fixture for ObjectManager replies and device notifications.
`python-dbusmock` was checked: it is not installed here and its supplied
templates do not include UDisks2. The fixture adds only the behavior required
for this checkpoint; do not expand it into a general service emulator.

Exercise enumeration, additions/removals, property changes, service loss,
reordered asynchronous replies and device replacement at a reused object path.
Check the visible selection and actions as well as the service calls. Once
authorization exists, test approval, denial, cancellation, absent services and
device removal while awaiting a reply. These simulate service results; native
authentication-agent behavior needs guest desktop integration checks.

### Temporary images and I/O

Use `QTemporaryDir`/`QTemporaryFile` and small generated fixtures with explicit
size limits. Compare output bytes independently, including lengths, untouched
regions and final partial blocks. Add empty input, source/target identity,
capacity, truncated input, cancellation, short I/O and failed flush cases as
the corresponding production paths appear. Use a narrow named I/O test hook
only where a failure cannot be produced reliably with ordinary files; do not
build a general filesystem abstraction just for tests.

For shared image analysis, record the fixture source/license, expected report
and the Rufus revision/behavior supporting that expectation. Keep large external
images outside Git, pin their checksums and document how to obtain them. Run
formatting and extraction through the application's platform adapter once it
exists, then independently inspect the result. Merely testing `mkfs` on its own
does not verify our integration.

Loop devices are an optional later block-device fixture, preferably inside the
VM. A loop mapping is not USB emulation. Default tests need no host loop setup,
root privileges or access to host disks.

### QEMU USB and boot tests

Use an ordinary disposable image as the storage backing for QEMU's `usb-storage`
device on an emulated XHCI controller. The guest sees a USB mass-storage device.
This provides a practical route to real UDisks2 operations without physical
USB hardware. [QEMU USB documentation](https://www.qemu.org/docs/master/system/devices/usb.html).

At CP6, add a reproducible guest runner with a pinned Linux image, separate guest
system disk and test target, and an unprivileged desktop user. Attach only
disposable image files as test storage, never host device nodes or USB
passthrough. Perform any elevated storage work inside the guest. Record QEMU,
guest, service and firmware versions. Use hardware acceleration if available,
with TCG as the slower fallback and appropriate bounded timeouts.

First prove that guest discovery identifies the emulated USB device and that
monitor-driven detach/attach causes the expected selection changes. Confirm
guest-observed removal, rather than relying only on a monitor command returning.
Then run the application's storage path against that device and inspect its
output after orderly shutdown. Add approval/denial and actual desktop prompt
checks in this environment, keeping the application unprivileged and passwords
out of its process, logs and test reports.

Boot tests use the resulting media in a separate VM after the writer has closed
it. Begin with a small redistributable fixture that emits an unambiguous serial
marker. Later installers need a specified boot screen/marker and evidence of
reaching it. Capture serial output or screenshots, enforce a timeout, and fail
if the expected stage is absent. Starting QEMU successfully is insufficient.
Track BIOS and UEFI results separately; test Secure Boot only with an explicit
firmware/key configuration. Retain real USB checks as a release qualification
task when hardware becomes available.

## Checkpoints

The table tracks implemented boundaries and remaining workflow steps. Following
the 2026-09-14 priority change, pause additional CP3 image variants and finish the
raw writing workflow. CP5's file operation and CP4/CP7's planning/authorization
components are tested. CP8a connects the GUI against simulated storage. CP8b
passes native USB qualification and enables application/device writing. CP9b
adds real Linux BIOS/UEFI boots and live reattachment coverage.
CP10a/CP10b preserve Rufus's ISOHybrid policy and dialog. CP10c starts the ISO
execution path with ordinary-file extraction into disposable directories.
CP10d formats an explicit FAT32 region in an image file with native tools;
CP10e creates and verifies an ordinary GPT main partition, then formats its
actual extent. Device integration remains separate.

| ID | Bounded change | Required exit evidence | Status |
| --- | --- | --- | --- |
| CP0 | Align the reporting prototype with the coding rules; establish this test plan. | C caller reaches real Widgets; all reporting regressions pass, including routing/lock cleanup after exceptions. | Complete |
| CP1 | Widgets shell matching the Win32 GUI and selection state, using synthetic records in tests. | QtTest operates image/device controls and log view; empty/replaced selections behave correctly; layout and advanced sections follow Win32; writing remains unavailable. | Complete |
| CP2 | Read-only asynchronous UDisks2 discovery. | Private-bus tests cover enumeration, hotplug, stale replies, stable identity and service failure; read-only host smoke check requires no USB. | Complete |
| CP3 | One useful shared Rufus image-analysis path. | A real image fixture produces an expected report using identified original C modules; malformed/truncated input fails clearly; reuse/adapters recorded. | ISO/UDF, embedded EFI and installation-WIM analysis reach SELECT. Further standalone-WIM integration, Secure Boot and broader variants are deferred; see CP3g4b below |
| CP4 | Write-plan creation and validation for the first raw-image path. | Fixture-driven checks cover capacity, identity, supported mode and invalidation after input/target changes. No writes. | Implemented for the narrow `.img` path; GUI connected in CP8a |
| CP5 | Raw writes and verification against regular files. | Exact output and final partial block; cancellation, short I/O/error handling, flush failure and bounded memory verified. | Implemented; authorized block entry point also tested through Widgets with simulated kernel facts |
| CP6 | Reproducible virtual USB fixture. | Linux guest observes the file-backed USB target; app discovery and selection react correctly to attach/detach. No host disks involved. | CP8b passes startup discovery/unplugging; CP9b passes live reattachment at reused device/UDisks paths and rejection of stale selection/plans |
| CP7 | Native authorization and target revalidation. | Service tests cover denial/cancel/unavailability and changed target; guest desktop checks show native interaction while UI remains unprivileged. | Private D-Bus contracts and CP8b native KDE/polkit prompt, unprivileged writing and target checks pass |
| CP8a | Qt raw-write controller and worker. | Actual Widgets confirm, authorize, write, flush and verify disposable storage; cancellation, failures, invalidation and worker cleanup have accurate states. | Implemented; private-bus tests and CP8b guest qualification pass |
| CP8 | Integrate the raw-image path with virtual USB storage. | App writes/verifies an explicitly selected guest target; failures, removal and cancellation produce accurate state and cleanup. | CP8b passes; the application raw writer is enabled |
| CP9 | Boot the first supported generated raw image. | Separate VM reaches the fixture's expected boot marker, with the tested firmware and image recorded. | Generated BIOS fixture passes; CP9b also boots written OpenWrt BIOS/UEFI raw images to userspace. Other distributions, OS installation and Secure Boot remain |
| CP10a | Share Rufus's boot-marker check and classify ISOHybrid candidates through SELECT. | Marker/short-read/source-change tests; generated ISO Widgets tests distinguish DD capability from required DD and do not enable unimplemented writing. | Implemented; verification recorded below |
| CP10b | Share ISOHybrid choice ordering and implement the original dialog in Qt Widgets. | Original message/default checks, all choice mappings, persistence/DD exceptions, cancellation and enlarged-font layout. Application ISO START remains gated. | Implemented; verification recorded below |
| CP10c | Native ordinary-file ISO/UDF extraction using shared traversal and readers. | Generated trees match names/content; unsupported entries, cancellation, changed sources, failed I/O and cleanup have checked results. No devices or mounts. | Implemented; verification recorded below |
| CP10d | Native FAT32 formatting in a specified region of a regular image file. | Requested geometry/label/ID, valid FATs and backup metadata, independent fsck/mtools checks, surrounding-byte preservation, cancellation/error/child cleanup. | Implemented; verification recorded below |
| CP10e | Share Rufus placement arithmetic and prepare one ordinary GPT main partition using libfdisk. | Original-formula comparisons; raw GPT headers/entries/CRCs and protective MBR checks; formatting of the actual extent preserves both GPT copies; failure/cancel/ownership checks. | Implemented; verification recorded below |
| CP10+ | Extraction, filesystems and additional boot/media support. | Define a separate checkpoint for each partitioning, formatting, extraction or boot feature, including output inspection and applicable boot checks. | Unscheduled |

Linux CI was added with CP1: run the available `no-hardware` suite on every change.
Add service and file tests to that suite as they land. Keep slower `vm` and
`boot` jobs explicit, with their fixtures and dependencies documented. Add CI
coverage for the declared minimum Qt version as well as the development version.
Do not require network downloads during an ordinary test run.

## Checkpoint records

**CP0 complete, 2026-09-13.** `RufusJob` now has a named protected `execute()`
entry point for concrete workers. Its `run()` slot retains serialization and
report lifetime. Production code and tests have no lambdas or `std::function`;
useful optional values, Qt containers and RAII ownership remain. The tests use
named receivers and preserve all seven original behavioral cases, adding an
eighth for routing and lock recovery after an unknown exception.

Validated with Qt 6.11.2 and GCC 16.2.1:

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure -L no-hardware
```

The build passed without compiler warnings. CTest passed its one reporting
suite: eight behavioral cases, plus QtTest initialization and cleanup. Labels
are `component`, `widgets` and `no-hardware`. Original Windows sources and build
files are unchanged. Work remains local and uncommitted on
`codex/linux-qt6-reporting`.

**CP0 verification rerun, 2026-09-13.** Reconfigured the normal Debug build and
ran the complete CTest suite: all eight behavioral cases passed. Also built
the project from scratch in `/tmp/rufus-linux-qt6-sanitizers.ssKskZ` with
`-fsanitize=address,undefined -fno-omit-frame-pointer` for C/C++ compilation and
`-fsanitize=address,undefined` for executable linking. The same suite passed
with address, undefined-behavior and leak checks enabled, with no sanitizer
diagnostics:

```sh
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /tmp/rufus-linux-qt6-sanitizers.ssKskZ --output-on-failure -V
```

The initial sanitizer run passed its assertions but LeakSanitizer could not
finish under the execution sandbox's tracing restrictions. The rerun outside
that sandbox completed successfully with leak detection still enabled. The
project's C/C++ code was instrumented; installed Qt libraries were not rebuilt.
No production-code changes were needed. Scope remains the reporting component,
including real Widgets delivery; this does not add application or media tests.

Host readiness checked: QEMU 11.1.1 is installed with TCG support; `udisksctl`,
`dbus-run-session`, `sfdisk`, `mkfs.fat` and `losetup` are available. The current
environment exposes neither `/dev/kvm` nor `/dev/loop-control`; `qemu-img`,
`qemu-nbd` and Python's `dbusmock` module were not found. These are observations,
not an exhaustive dependency check. No guest fixture has been prepared and no
VM, disk-write or boot test has run.

### CP1 — Complete, 2026-09-13

Added `rufus-linux`, a Qt6 Widgets application, and a concrete selection
controller. The [form](../linux/qt/main_window.ui) follows the original Win32
GUI: section headings, Boot selection combo, paired fields, toolbar order,
START/CLOSE, initially collapsed advanced sections and the Clear/Save/Close log
dialog. The [control mapping](linux-win32-ui.md) records source references and
the remaining platform/capability differences. Original icons are reused at
their supplied resolutions. No original Windows source or build file changed.

Image selection uses the platform file picker and checks for a readable,
nonempty regular file. No analysis occurs yet. Synthetic device records remain
in tests. Selection survives reordering but is cleared on removal, replacement,
changed identity/path/size or ambiguous identity. Internal and read-only records
are excluded. All media operations remain disabled. The controller has no
storage-service connection and the application does not discover real devices.

Validation with Qt 6.11.2/GCC 16.2.1:

- Normal Debug build passed without compiler warnings.
- All three CTest entries passed: eight reporting cases, twenty Widgets
  cases/data rows, plus the large-font layout case repeated at 2× scaling.
- Tests operate actual controls with mouse/keyboard events, accept/cancel image
  selection, save/clear/reopen the log, verify Win32 captions/order/expansion,
  and check Enter/Escape and window teardown. File pickers use Qt Widgets in
  tests; native desktop pickers still need manual platform checks.
- Address, undefined-behavior and leak sanitizer runs passed all three entries
  with no diagnostics. As at CP0, leak checks ran outside the tracing sandbox;
  installed Qt libraries were not rebuilt with instrumentation.
- A separate `BUILD_TESTING=OFF`, `RUFUS_BUILD_GUI=OFF` build passed, retaining
  the Qt Core-only reporting component.
- Collapsed/expanded screenshots were inspected. The font-size test waits for
  Qt's deferred geometry update with a bounded condition before assessing fit.
- The additive [Linux CI workflow](../.github/workflows/linux-qt6.yml) targets
  Ubuntu 22.04/24.04 and its YAML parsed locally. It has not run on GitHub, so
  this is not yet evidence for minimum-Qt compatibility.

Reproduce using the normal build/test commands above. Launch the preview with:

```sh
/tmp/rufus-linux-qt6-build/rufus-linux
```

The sanitizer directory is `/tmp/rufus-linux-qt6-sanitizers.ssKskZ`; the isolated
Core-only build is `/tmp/rufus-linux-qt6-core-cp1`. Optional test-only environment
variables `RUFUS_TEST_SCREENSHOT` and `RUFUS_TEST_EXPANDED_SCREENSHOT` save form
captures to explicitly supplied paths. No simulated USB, media-write or boot
validation has run. Work remains local and uncommitted.

### CP2 — Complete, 2026-09-13

Added a concrete Qt D-Bus reader and connected it to the existing controller,
device combo and status/log areas. ObjectManager discovery is asynchronous;
hotplug and property changes refresh the list. Removal, replacement, service
loss and relevant metadata changes invalidate selection. Superseded replies
cannot restore obsolete records. Requests have a five-second timeout, transient
failures retry after two seconds, and service reappearance triggers a refresh.
Loss of the bus clears the list and asks for an application restart.

The initial USB filter and identity limits are recorded in the
[Linux README](../linux/README.md#read-only-device-discovery). The reader opens
no device nodes and performs no authorization or storage operations. The Win32
form and original Windows sources/build files remain unchanged. No new authored
templates, lambdas or callback framework were introduced; Qt containers represent
the nested ObjectManager wire signature directly.

Validation with Qt 6.11.2/GCC 16.2.1:

- Normal Debug build passed without compiler warnings. All four CTest entries
  passed: the existing reporting/Widgets/2× layout suites and 31 new discovery
  cases/data rows. The normal run took about 12 seconds.
- Each discovery case starts a private `dbus-daemon` and a separate Qt fixture
  process. Tests cover the wire format and filtering, real window selection,
  hotplug, same-path replacement, media/identity changes, out-of-order replies,
  service disappearance/reappearance, malformed replies, error/retry, timeout,
  disconnected/lost buses and UI responsiveness while a reply is held.
  Unexpected storage-service calls fail the tests.
- Address, undefined-behavior and leak checks passed all four CTest entries
  in `/tmp/rufus-linux-qt6-sanitizers.ssKskZ`, with no diagnostics. Installed Qt
  libraries were not rebuilt with instrumentation. Fixture processes exit
  normally for leak checking except in the deliberate bus-loss case, where
  the disconnected fixture is terminated during cleanup.
- A read-only run of `/tmp/rufus-linux-qt6-build/udisks_probe --system` succeeded
  with **0 eligible USB devices**, matching the lack of USB hardware. It
  exercised the production reader against the real UDisks2 service without
  opening device nodes or requesting authorization.
- The Core-only reporting configuration still builds. Whitespace checks and
  Linux CI YAML parsing passed; CI now installs `dbus-daemon`. GitHub CI and
  minimum-Qt validation remain unrun.

The execution sandbox disallows Unix socket creation and access to the host
system bus. Private-bus tests and the host probe therefore ran outside that
sandbox, still as the ordinary user; no root access was used. LeakSanitizer also
required the same tracing-restriction exception as earlier checkpoints.
These are execution-environment requirements, not application privileges.

Reproduce with the normal build/test commands above and the explicit smoke
probe in the README. No physical/virtual USB, native authentication, media-write
or boot validation has run. Work remains local and uncommitted on
`codex/linux-qt6-reporting`.

### CP3a — Source/adapter investigation, 2026-09-13

The user requested a detailed discussion of sharing boundaries and favors
Linux adapters that preserve Rufus's existing design. The
[boundary document](linux-image-analysis-boundary.md) records an adapter-first
direction, the actual scan call chain, common policy and the limits that would
justify a small source move. The user explicitly excluded Wine and Winelib:
compatibility functions must use native Linux/POSIX APIs.

A standalone [libcdio probe](../linux/probes/libcdio/README.md) compiled 20
original C files unchanged using a Linux/glibc configuration. The executable
read a generated ISO through a Unicode path, with the expected volume label,
file size and payload prefix. A comparison against the installed system 2.4.0
library established a meaningful difference: only the bundled reader resolved
Rufus's synthetic El Torito `[BOOT]` path. Both rejected empty, plain-text and
descriptor-truncated inputs. Correct library-specific API/structure layout was
required for the comparison. Existing bundled warnings remain recorded.

This is investigation evidence, not CP3 completion. It introduces no application
behavior, shared-source changes or new default build dependencies. Existing
application tests were not rerun because their code and build targets did not
change. No UDF-positive, sanitizer, Windows, authorization or boot test was run
for this experiment. All work remains local and uncommitted.

The next user decision required separating mixed GUI/media code in Rufus itself.
That supersedes the earlier plan to measure a whole-scan bridge before moving
source. Small native API adapters remain appropriate for platform operations.

### CP3b — Shared post-scan policy, 2026-09-13

Moved the existing report and neutral flags from `rufus.h` to
`src/image_report.h`. Extracted Red Hat/SUSE label rules, EFI GRUB filesystem
requirements and ISOHybrid DD decisions from the Win32 scan thread into
`src/image_policy.c`. Windows calls the shared policy and retains its existing
dialog and `is_iso` update order. VS and MinGW source/build lists are updated.
Qt form code and the application workflow have not changed in this increment.

Validation:

- Standalone C11 build passes with no compiler warnings. All 199 policy cases
  pass, using the original three `cregex` C files. Tests cover labels and version
  boundaries, UTF-8, filesystem masks, large-file encodings, boot methods,
  DD detection states, previous findings, repeatability and unrelated report data.
- The same 199 cases pass in a temporary harness of the original policy block
  extracted from `18ae93bf37f94764d77216cff4168e0549a14c4d`. This characterizes
  media decisions; it does not execute Windows dialogs.
- A mechanical comparison confirms moved definitions retain their content
  except `BOOLEAN` becoming `uint8_t` and trailing whitespace. Compile assertions
  preserve report size/alignment and all 48 field sizes/offsets on Linux and
  Clang's freestanding Windows x86/x64 targets. No Windows runtime was used.
- Normal Linux CTest: **5/5 passed**, about 12 seconds. Includes existing Widgets
  and private-D-Bus functional tests plus the new shared policy test.
- AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer: **5/5 passed**,
  about 14 seconds. The same execution-sandbox exceptions for private sockets
  and LeakSanitizer as CP2 were used; tests remained unprivileged.
- `git diff --check` passes. Windows project CRLF line endings are preserved.

The [standalone test instructions](../tests/image_policy/README.md) require no
Qt, USB, images, mounts or authorization. The Linux CMake build includes them
automatically. An additive Windows x86/x64 component workflow is prepared but
has not run. Full native VS/MinGW application builds and a GUI behavior check
remain pending; no native Windows toolchain/runtime is available here.

This completes the first source split and its local tests, not CP3's image
analysis integration. `ApplyIsoPolicy` requires a complete report; a zeroed
partial Linux scan is not sufficient. START remains disabled. No media-write
or boot validation has run. Work remains local and uncommitted.

**Next: CP3c, in small increments.** Separate per-file ISO classification from
extraction and GUI work, then exercise shared rules against a generated image
through the bundled reader. Preserve the existing Windows calls and obtain
native Windows validation for shared-source changes before proposing upstream.
Authorization and media creation remain later checkpoints.

### CP3c1 — File classification and generated ISO fixtures, 2026-09-13

Separated per-file classification from the mixed scan/extraction function into
`src/image_file.c` and `src/image_file.h`. The Windows wrapper retains its path
lists, total block count, skip decisions and WIM splitting. Shared code has
explicit report/file-property arguments and no GUI, device or allocation
dependency. Small CRT string mappings are selected by the Linux build.

Validation:

- Standalone C11 classifier: **152 checks passed**, without compiler warnings.
- Temporary before/after harness: **44,280 combinations matched** report bytes,
  list counts, block totals, skip results and recorded WIM split arguments.
  Scan mode now also fills three previously unused extraction-property flags;
  that deliberate contract change is normalized in this comparison.
- Main Debug CTest: **6/6 passed** (about 12 seconds).
- Main ASan/UBSan/LeakSanitizer CTest: **6/6 passed** (about 14 seconds).
- Separate generated-ISO fixture: **1/1 passed** normally and under the same
  sanitizers. Tests classify 17 entries from two Rock Ridge images through the
  bundled reader, verify filename case and symlink data, and reject ordinary
  invalid inputs, a missing path and a directory passed as a file.

The fixture sanitizer test exposed two pre-existing bundled reader issues.
The stat-clone paths now copy symlink data only when present, and the ISO
32-bit decoder shifts the high byte as unsigned. These are small common-source
fixes, covered by ordinary file and positive symlink fixtures. Remaining bundled
compiler warnings are recorded in the probe documentation; no broader parser
audit was attempted.

Both Windows build manifests are updated. Portable Windows CI now covers policy
and file classification; Linux CI includes the standalone ISO fixture. CI has
not run. Full Windows builds and GUI behavior checks remain pending.

Reproduce using [classifier test instructions](../tests/image_file/README.md)
and [ISO fixture instructions](../linux/probes/libcdio/README.md). The fixture
looks up known paths; it is not a second image walker or a complete scan result.
Joliet/UDF traversal, image-analysis job integration, media writes and boot
validation remain outside this increment. START remains disabled.

The user's native-reuse clarification is captured in
[Linux native reuse](linux-native-reuse.md). Prefer a native Syslinux installer
where payload versions, filesystem and sector requirements match; use native
wimlib and filesystem operations as already planned. The bundled installer is
a fallback to assess, not a prerequisite to port. No installer was run.

**Next: CP3c2.** Isolate one traversal/scan-finalization slice using these shared
file rules, retain the existing name/symlink behavior, and test a generated
image before connecting it to the GUI. Work remains local and uncommitted.

### CP3c2 — Shared read-only ISO tree scan, 2026-09-13

Extracted ISO9660 scan traversal from the mixed extraction function into
`src/iso_scan.c`. Both the Windows scan caller and the Linux test driver use
this implementation. The scanner takes explicit report/path arrays and options;
it preserves directory order, RR/Joliet name handling, long-name and symlink
flags, firmware extra-block allowance, Mint's NTFS rule and Rufus's deep-directory
short circuit. Progress initialization remains in the Windows caller.

Moved the original `StrArray` out of `stdfn.c` / `rufus.h` into a neutral module,
using native Windows CRT calls or the small POSIX mapping chosen by Linux CMake.
Growth failure now preserves the old allocation, capacity and owned strings for
cleanup/retry. The new scanner checks failed path insertion. Overlong tree paths
fail explicitly instead of silently truncating names; block rounding avoids
addition overflow. No extraction buffer is allocated by the scan-only walker.

The bundled reader now has one definition in `linux/cdio/`, shared by the Linux
build and standalone comparison probe. Its previous host-specific configuration
was replaced with CMake feature checks for native headers/functions, time
structures, byte order, iconv and 64-bit offsets. Public ABI/offset definitions
propagate to consumers. The original stdio/iconv implementations perform image
I/O; no Windows file or GUI emulation was added.

Validation on the existing Linux host:

- **8/8 main CTest suites pass**, including all earlier Qt and private-bus tests.
- **8/8 pass with address, undefined-behavior and leak checks**, with no sanitizer
  diagnostics. Runtime was about 12.5 seconds normally and 14.3 seconds sanitized.
- The new tree suite scans five generated ISOs. Its 39-file mixed tree checks
  original rules, case, two growing path arrays, 2048-byte rounding and symlinks.
  Further fixtures check RR/Joliet/plain names, empty trees, overlong paths and
  deep-directory partial results. Four cancellation points and missing, empty,
  text, directory and descriptor-truncated inputs exercise cleanup/failure.
- The new string-array test checks copied/owned strings, uniqueness, failed
  growth, retry, clear/reuse and destruction. Failed `realloc` is injected only
  by the Linux test linker; the production interface has no test hook.
- The standalone 17-entry classifier fixture and bundled/system comparison
  still pass after sharing the new CMake target.
- Source registrations and VS CRLF are consistent; `git diff --check` passes.

Main test command:

```sh
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure --no-tests=error
```

The sanitizer build uses the same flags and execution setup recorded in CP0/CP2.
Its private D-Bus sockets and LeakSanitizer require leaving this execution
sandbox, but tests still run as the ordinary user with temporary regular files.
No host media, mounts or authentication requests are involved. Existing bundled
compiler warnings remain; CI has not run. Native Windows validation is deferred
as requested and does not block the next Linux increment.

A completed tree remains **partial image analysis**. The driver labels it that
way and reports deep-directory scans separately. Cancellation is cooperative
between entries/reads, not an I/O deadline. UDF traversal, content reads,
bootloader inspection and scan finalization are not yet in this Linux path.
The Widgets form is unchanged and SELECT still only selects a file; START
remains disabled.

**Next: CP3c3.** Separate shared image-file content reading from extraction/GUI
work, with small fixtures for contents and errors. Then move the required
bootloader inspection and finalization in further bounded increments before
connecting the result to a concrete Qt job. UDF needs its own traversal fixture.
Work remains local and uncommitted.

### CP3c3 — Shared image-file buffer reads, 2026-09-13

Moved the existing `ReadISOFileToBuffer` implementation from `iso.c` into
`src/iso_read.c`, with an explicit ISO extension-mask argument and private
ISO9660/UDF helpers. Windows keeps the original signature in a small wrapper
that supplies its preferences and preserves log-level setup/restoration. Linux
builds the same reader in `rufus_iso_scan`, using the existing bundled stdio and
iconv implementations. No GUI callback, platform emulation or new dependency
was introduced.

Preserved contracts: UDF-first lookup, ISO fallback only when UDF open fails,
name-extension selection, the inclusive 1 GiB limit, bulk reads, original byte
count and caller-owned buffer with an extra NUL. Empty files and errors still
return zero/null. Directory input now fails explicitly; null arguments and
unsigned lengths are checked before access/narrowing. Empty files skip allocation.
Individual reads remain synchronous, as in Rufus.

Validation:

- **10/10 Linux CTest suites pass**, about 12.7 seconds, including earlier Widgets,
  private-D-Bus, tree and policy tests.
- **10/10 pass with address, undefined-behavior and leak checks**, about 15.2
  seconds, with no sanitizer diagnostics. The same unprivileged execution setup
  documented for prior checkpoints was used.
- Generated ISO content tests compare complete binary payloads at sizes 1, 2047,
  2048, 2049 and 70001 bytes, embedded NULs, RR/Joliet/plain paths and a Unicode
  image filename. They check the synthetic El Torito entry, internal missing
  paths/directories, empty files, invalid images and ordinary truncated file data.
  The boot catalog explicitly declares eight 512-byte sectors for its 4096-byte
  synthetic payload; this avoids relying on the image writer's defaults.
- A separate API-level test covers **24 read scenarios plus null arguments**:
  both format branches, UDF preference without fallback after lookup/root failure,
  size bounds, allocation failure, short/error reads, resource cleanup and retry
  within the process. Link-time allocation failure prevents a real 1 GiB allocation.
- Source registrations and VS CRLF are consistent; `git diff --check` passes.
  Existing bundled zero-array warnings remain visible. CI has not run.

Reproduce with the usual build/full CTest commands, or select the new tests:

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_iso_contents|rufus_iso_read_contract' --output-on-failure
```

The UDF tests use API doubles, not an on-disk UDF image. No UDF mastering tool
is installed here; a real UDF read/traversal fixture remains required. These
checks do not validate bootloader interpretation or bootability. The Widgets
form and file-selection behavior are unchanged; START remains disabled.
Native Windows validation stays deferred to real Windows as requested.

**Next: CP3c4.** Share Rufus's Syslinux version detection and test it with
contents read through this path. Native Linux Syslinux installation remains the
preferred later backend where version/filesystem requirements match; sharing
version recognition does not commit us to porting the bundled Windows installer.
Then handle GRUB/EFI inspection and scan finalization in further bounded steps.
Work remains local and uncommitted.

### CP3c4 — Shared Syslinux version detection, 2026-09-13

Moved `GetSyslinuxVersion` and its major/minor macros into
`src/syslinux_version.c/.h`. Linux and Windows now select the same ordinary C
implementation. Windows installation remains in `syslinux.c`; no installer API
or new platform adapter is required by the parser. Its signature and borrowed,
normalized suffix are preserved. The image caller adds the reader's terminating
byte to the supplied span; embedded resources keep their actual lengths.

The parser retains first-usable-marker selection, SYS/ISO prefixes, search
margins, version encoding, duplicate-version removal, suffix trimming and
character substitutions. Deliberate contract changes check the supplied span's
termination, require decimal major/minor fields separated by a dot, and reject
out-of-range fields without assertions. A bare version retains its terminating
NUL. The parser allocates nothing. It caches a known terminator across candidates
and uses linear suffix scans, avoiding repeated length/search work.

Validation:

- **12/12 Linux CTest suites pass**, about 12.6 seconds, including all prior
  Widgets, private-D-Bus, image and reporting tests.
- **12/12 pass with address, undefined-behavior and leak checks**, about 15.2
  seconds, with no sanitizer diagnostics. Tests use the same ordinary-user
  execution setup recorded for earlier checkpoints.
- **46 component cases** cover version/suffix behavior, minimum span and search
  boundaries, invalid/unterminated input, first usable marker and borrowed-buffer
  ownership. Both bundled binaries are read as data: v4.07 `/2013-07-25` and
  v6.04 `/pre1`, with provenance retained in `res/syslinux/readme.txt`.
  After the full-suite runs, four cases were added to preserve the original
  numeric whitespace/plus handling; both affected suites passed again normally
  and with sanitizers.
- A generated ISO checks the content-reader/parser chain for both resources,
  synthetic distro banners, termination supplied at EOF, unknown versions,
  small/empty files and ordinary read failures. No loader or installer executes.
- The original parser, isolated in a temporary Linux C harness, produces the
  same generated-image results. This compares fixture behavior, not all inputs
  or native Windows operation. The new contract tests run against shared code.
- The standalone C test project also builds/passes without Qt or libcdio.
  Source registrations and VS CRLF are consistent; `git diff --check` passes.
  Prepared CI covers the new tests and asset paths but has not run.

Reproduce this increment's tests:

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_syslinux_' --output-on-failure
```

For independent C builds, see [the parser test instructions](../tests/syslinux_version/README.md).
No new compiler warnings were introduced by this module. Existing bundled-reader
warnings remain. Windows validation is still deferred to real Windows.

**Agreed follow-up:** keep detection/presence separate from installation
selection, reuse Rufus's v4/v6 fallback as an explicit inferred choice, and
select a native installer capable of installing the chosen payload. Unknown
detection alone does not reject an image. The [plan](linux-port-plan.md#syslinux-detection-and-installation-selection--agreed-2026-09-13)
records policy tests and disposable-image/QEMU fallback tests. CP3c4 did not
implement selection; the Windows fallback/conflicting-loader rules remain.

**Next: CP3c5.** Separate GRUB version/filesystem inspection from its global report
and path-list bookkeeping, then test it using shared content reads. EFI inspection,
scan finalization, a real UDF fixture and Qt job integration remain later bounded
increments. START remains disabled. Work remains local and uncommitted.

### CP3c5 — Shared GRUB inspection, 2026-09-13

Moved GRUB version/build and filesystem inspection from `iso.c` into
`src/grub_inspect.c/.h`. Callers now supply their report and string array
explicitly. The label sanitizer moved from `parser.c` into this module as a
private helper; its implementation is preserved apart from linkage and array
length syntax. The shared functions use the existing host `uprintf` endpoint.
Windows retains traversal, file choice and UI/report presentation. The original
FAT/exFAT/NTFS mask mapping is now shared too.

Retained behavior includes both banner spellings, the CentOS newline skip,
last usable marker within one file, preservation of an existing report version,
zero-version exclusion, and ordered nonstandard-prefix/debug/2.06-label
suffixes. Filesystem names retain their order, case-sensitive uniqueness and
11-character limit, with case-insensitive report-mask mapping. No new Linux API
adapter or dependency was needed.

The input contract is an explicit accessible byte span: version strings must
terminate inside it, output remains capped at 191 bytes, and callers include
the reader-added NUL. Overlapping candidates reuse a known terminator. Filesystem
allocation failure returns false while retaining collected names; Windows logs
that failure. These changes bound the shared operations without introducing
GUI or installer responsibilities.

Validation:

- **14/14 Linux CTest suites pass**, about 12.7 seconds, including existing
  reporting, Widgets, private-D-Bus and image tests.
- **14/14 pass with address, undefined-behavior and leak checks**, about 15.6
  seconds, with no sanitizer diagnostics. Tests run as an ordinary user with
  temporary files and private sockets, as at earlier checkpoints.
- **38 Linux component cases pass:** 36 portable cases plus allocation-failure
  and retry checks using Linux linker injection. Input buffers stay unchanged;
  copied filesystem names survive buffer reuse.
- An **eleven-file generated ISO** exercises shared reads followed by GRUB
  inspection, including version choice across files, filesystem union, supplied
  EOF termination, unknown versions and ordinary read failures. Files are
  synthetic marker data, not executable GRUB modules or PE images.
- A temporary harness of the original GRUB functions passes the same generated
  image expectations. This establishes fixture compatibility, not native Windows
  validation or all-input equivalence. New boundary/failure tests run only
  against the shared implementation.
- The standalone C project builds and passes without Qt/libcdio. Source
  registration, unchanged sanitizer logic and existing VS/parser CRLF checks
  pass. `git diff --check` passes. The new code builds without warnings.
  CI paths and the native C matrix are updated but have not run remotely.

Reproduce this checkpoint with:

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_grub_' --output-on-failure
```

See [component instructions](../tests/grub_inspect/README.md) and the
[inspection boundary](linux-image-analysis-boundary.md#cp3c5-shared-grub-inspection).
The agreed Syslinux presence/detection/fallback distinction is recorded in the
[main plan](linux-port-plan.md#syslinux-detection-and-installation-selection--agreed-2026-09-13);
this checkpoint does not implement installer selection.

**Next: CP3c6.** Examine and separate required EFI content inspection with its
PE-reader dependencies and ordinary-file fixtures. Scan finalization (including
the agreed Syslinux policy split), real UDF fixtures and Qt job integration
remain later checkpoints. Real GRUB/EFI fixtures and QEMU boot validation are
still required for installation coverage. START stays disabled; Windows
validation remains deferred. Work is local and uncommitted.

### CP3c6 — Shared EFI banner diagnostics, 2026-09-14

Moved `GetEfiBootInfo` from `iso.c` into `src/efi_boot_info.c/.h`. Source review
corrected the earlier dependency assumption: this function only reports Shim
and systemd-boot banner versions. It has no PE-reader dependency and does not
change image-report flags. `GetPeSection` belongs to separate SBAT/revocation
work under the Windows Secure Boot workflow; that work remains deferred.

The same C function is selected by both builds, retaining marker order, the
first matching banner, 64-byte search margin, 32-byte version limit, whitespace
handling and diagnostic text. The input is now const: bounded printf precision
replaces writing a NUL into the caller's buffer. The ctype argument is converted
to an unsigned byte. The caller still passes the actual byte count; no extra
termination byte or Linux adapter is needed. Reporting uses the existing host
`uprintf` endpoint, with no allocation or new callback interface.

Validation:

- **16/16 Linux CTest suites pass**, about 12.3 seconds, including the existing
  reporting, Widgets, private-D-Bus and image tests.
- **16/16 pass with address, undefined-behavior and leak checks**, about 22.9
  seconds. No sanitizer diagnostics. Tests run as an ordinary user with private
  sockets and temporary files; no physical USB device is accessed.
- **34 standalone C cases pass**, checking exact messages, input immutability,
  marker/order/token rules, search bounds, read-only and nonterminated spans,
  unsigned-byte handling and null/short inputs. No Qt/libcdio dependency.
- **Nine synthetic files in a generated ISO** exercise the content reader plus
  banner reporting, along with empty/missing/directory/plain-image failures.
- **Two installed systemd-boot binaries**, x64 and IA32 version `261.3-1-arch`,
  pass through the same generated-image test normally and with sanitizers.
  Their independently inspected banners, sizes, hashes and reproduction command
  are recorded in the [test instructions](../tests/efi_boot_info/README.md).
  These files were only read as data and copied into disposable fixtures; they
  were not added to the repository, installed or executed.
- A temporary harness of the original function produces the same synthetic and
  real-file image diagnostics. This establishes fixture compatibility, not
  all-input equivalence or native Windows validation. New contract tests run
  only against shared code.
- Windows source registration and VS CRLF checks pass. CI path filters and the
  native C test matrix include this module, but remote CI has not run.
  `git diff --check` passes. Build warnings remain confined to the existing
  bundled libcdio reader; the new module and tests introduce none.

Reproduce this increment from the main Linux build:

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_efi_' --output-on-failure
```

This checkpoint covers version-banner diagnostics. It does not validate PE
structure, signatures, revocation, or actual boot behavior. No real Shim binary
was tested. The [boundary discussion](linux-image-analysis-boundary.md#cp3c6-shared-efi-banner-diagnostics)
records the corrected source dependency map.

**Next: CP3c7.** Separate configuration-path selection and Syslinux scan policy,
including explicit presence, detected version and inferred installation choice.
Keep the existing fallback and conflicting-loader rules shared. Remaining scan
finalization, embedded EFI/FAT inspection, real UDF fixtures and Qt integration
remain separate increments. START stays disabled; Windows validation remains
deferred. Work is local and uncommitted.

### CP3c7 — Shared configuration and Syslinux scan selection, 2026-09-14

Moved config selection, loader-conflict handling and detected/embedded-version
selection from `iso.c` into `src/syslinux_scan.c/.h`. The three functions use
explicit report/path arguments, copy config/suffix data, and borrow the chosen
source path only for reporting during selection. They allocate nothing and
perform no I/O. Windows retains its image reads and supplies its existing
embedded versions; Linux compiles the same policy code.

The agreed report distinction is now implemented:

- `has_syslinux` records configuration presence, including unknown versions;
  `HAS_SYSLINUX` uses it.
- `sl_detected_version` and `sl_detected_version_ext` retain the chosen image
  loader's detection result, without being replaced by a fallback.
- Existing `sl_version`/string/ext fields remain the selected installation
  version; `sl_version_inferred` identifies a nonzero fallback choice.

Rufus's shortest/latest-tie and openSUSE preferences, conflict-prefix behavior,
suffix limit, family warnings and v4/v6 fallback remain shared. A failed loader
read is skipped; an unknown successfully read candidate still participates in
the original preference rules. A supplied zero fallback version leaves selection
unavailable without erasing presence. Native installer/payload compatibility
must still be checked before any write operation.

The path-limit error is explicit: when no config fits the existing 127-byte
report capacity, the shared selector fails instead of retaining the old
underscore placeholder. Windows sets an ISO-scan error and the Linux probe
fails. The current path limit itself is unchanged.

Validation:

- **18/18 Linux CTest suites pass**, about 12.6 seconds, including the existing
  Widgets, reporting, private-D-Bus and image tests.
- **18/18 pass with address, undefined-behavior and leak checks**, about 16.3
  seconds, with no sanitizer diagnostics. Tests run as an ordinary user and use
  disposable files/private sockets, with no USB access.
- **29 standalone C policy cases pass**, covering config limits/preferences,
  candidate selection, suffix ownership, fallback origin, absence, stale state,
  unavailable versions and preserved conflict quirks. No Qt/libcdio dependency.
- **Two image-policy cases added** (201 total): unknown Syslinux presence is
  retained, and a selected version alone cannot invent presence.
- **17 generated ISO files** exercise the real shared traversal, reads, version
  parser and policy together. These include the bundled v4/v6 payload bytes as
  data, config-only and loader/module-only trees, conflicting and unknown
  preferred loaders, skipped empty loaders, suffix differences, mixed case and
  config-length boundaries. Unavailable fallback variants and a plain-file error
  also pass. No loader is installed or booted.
- The original selection block passes **16 matching supported-path images** in
  a temporary Linux harness. Its pre-fallback fields are copied into the new
  detection fields for comparison; the original choices remain unchanged.
  New error/unavailable-fallback behavior is excluded from that comparison.
  This is fixture compatibility, not all-input or native Windows validation.

Reproduce this increment:

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_syslinux_scan|rufus_image_policy' --output-on-failure
```

See [policy test instructions](../tests/syslinux_scan/README.md) and the
[source boundary](linux-image-analysis-boundary.md#cp3c7-shared-syslinux-scan-policy).
Windows source manifests and CI test/path coverage include the new module;
remote CI and real Windows validation remain unexecuted. Work is local and
uncommitted.
Source-registration and VS CRLF checks pass, as does `git diff --check`.
Build warnings remain confined to the existing bundled reader.

**Next: CP3c8.** Address remaining ISO9660 scan finalization and orchestration,
including explicit partial/error outcomes, before connecting the Qt job.
Embedded EFI/FAT inspection, Secure Boot analysis, real UDF fixtures and native
installer/boot tests remain separate increments. START remains disabled.


### CP3c8: shared ISO9660 finalization and coordinator (2026-09-14)

Added `AnalyzeIso9660` in `src/iso_analyze.c/.h`, combining the existing shared
ISO9660 tree, file reads and bootloader stages into one read-only operation.
The host supplies a validated regular-file path/size and simple name/fallback
options. The operation owns all temporary resources and returns a report,
counters, completed-tree flag and explicit pending reasons as plain data.
The direct reporting/cancellation entry points remain; no callback registry,
Qt types, generic adapters or privilege handling were added.

`FinalizeIsoScan` shares Rufus's label trimming, signed PVD/file-size mismatch
and exact float-based 1% projected-size rule, with arithmetic bounds checked.
`ScanSyslinuxBootloaders` and `ScanGrubAndEfiBootloaders` in
`src/iso_bootloaders.c/.h` share the original read/selection loops and order.
Windows calls these helpers while keeping its existing UDF, embedded-FAT,
WinPE/WIM, log preferences and GUI/extraction stages. Its optional-read behavior
is retained; storage failures/cancellation fail the scan.

The coordinator's result contract is explicit:

- **Complete:** the supported ISO9660 metadata stages finished. Raw/hybrid
  detection, final media policy, installer compatibility and boot validation
  remain separate requirements.
- **Partial:** findings are retained with reasons for UDF, deep directories,
  unavailable content, embedded EFI/FAT, Windows metadata, Secure Boot or a
  claimed volume larger than the file. Deep trees cannot publish a completed
  tree, projected size or selected bootloader version.
- **Cancelled or failed:** resources are released and the result is cleared,
  so a previous or half-filled report cannot look successful.

Recognized UDF containers return pending before any ISO tree scan; this prevents
mixing the ISO tree with UDF-first content reads. The coordinator uses one name
mask for its ISO tree and content. Unknown successfully read versions remain
valid unknown observations, distinct from unavailable content and inferred
Syslinux choices. The buffer reader's existing zero-return contract groups
empty files, allocation/size limits and I/O errors as unavailable content.
Individual library I/O calls remain synchronous.

Validation:

- **20/20 Linux CTest suites pass**, about 13.0 seconds, including Widgets,
  reporting, private-D-Bus and all previous image checks.
- **20/20 pass under address, undefined-behavior and leak checks**, about 17.2
  seconds, with no sanitizer diagnostics. Runs use ordinary-user temporary
  files/private sockets, without USB, mounts or root access.
- **22 disposable image/plain-file fixtures** exercise combined Syslinux,
  BIOS/EFI GRUB and main EFI diagnostics; filesystem-name union; first-version
  retention; config-only and unknown-version fallback; unavailable fallback;
  RR/Joliet/plain names; labels; projected sizes; firmware symlink extra blocks;
  partial metadata reasons; empty/non-image/truncated headers; volume mismatch;
  trailing file data; deep directories and path limits.
- The combined fixture tests cancellation before/during traversal and after
  each of five content reads. Each content read is independently failed, along
  with both path arrays, the GRUB filesystem array and a filesystem-name copy.
  Failed/cancelled results must be zeroed; sanitizer checks cover cleanup.
- **48 C contract checks pass** for metadata arithmetic, label edge cases,
  missing inputs, stale-result reset, UDF dispatch and cancellation around the
  UDF open. UDF APIs are doubled here: **real UDF decoding is still untested**.
- **Nine positive fixtures match the pre-split content loops**, comparing full
  probe reports and logs in a temporary Linux harness. This comparison excludes
  the new error/partial contracts and Windows-only metadata. It is not native
  Windows or boot validation.
- Source registration and Visual Studio CRLF checks pass, as does
  `git diff --check`. New compiler warnings are from bundled reader headers.

Reproduce this increment:

```sh
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build -R rufus_iso_analysis --output-on-failure
```

The Linux workflow already covers `src/**` and `linux/**`, so it picks up these
two new tests. Windows manifests include both modules; remote CI and real Windows
validation remain unexecuted. Work is local and uncommitted. See the
[analysis contract](../linux/README.md#shared-iso9660-analysis-operation) and
[source boundary](linux-image-analysis-boundary.md#cp3c8-shared-iso9660-finalization-and-coordinator).

**Next: CP3d.** Connect this supported ISO9660 operation to SELECT through the
existing concrete Qt job. Show complete/partial/error/cancelled status using
the existing Widgets form and log area; test cancellation, changing selection,
stale-result rejection and cleanup. START stays disabled. UDF traversal,
embedded EFI/FAT, Windows metadata, Secure Boot, raw/hybrid detection and native
installer/boot tests remain separate bounded increments.


### CP3d: SELECT runs the shared ISO9660 job (2026-09-14)

Connected SELECT to `ImageAnalysisJob`, a concrete implementation of the existing
`RufusJob`, running on a controller-owned worker thread. The job uses
`AnalyzeIso9660`; C algorithms continue to report through the existing entry
points. `IsIsoScanCancelled` now queries the current Qt thread's interruption
flag directly. A small scoped libcdio log handler routes its messages through
that same reporting path and restores the previous handler/level on every exit.

Results contain copied plain C analysis data and an explicit outcome/problem.
The shared result header no longer pulls the reader's structs into Qt headers;
scan result constants live with the neutral image report. Windows runtime code
and both form layouts are untouched by this checkpoint.

The controller assigns a new request number for every selection, including the
same pathname. Queued logs/results carry that number. Replacement invalidates
old reports immediately, interrupts the old worker and runs only the most recent
pending selection after the thread has stopped. Failure/cancellation clears the
report. Exceptions and the existing process-wide overlap guard also deliver
failed outcomes; `finished()` never means successful analysis.

The original form supplies the runtime behavior:

- SELECT starts an indeterminate scan. Device/image/advanced selection controls
  are disabled; CLOSE becomes CANCEL and the log remains usable.
- Cancel, Escape or main-window close requests interruption and keeps the main
  window open. After cleanup, CLOSE exits normally. Closing only the log does
  not cancel or destroy the job.
- The existing Volume label field displays a completed tree's label. Status/log
  text distinguishes complete ISO9660 metadata, partial checks, failure and
  cancellation. START and media settings remain disabled.

Fallback version numbers are parsed from Rufus's existing v4/v6 payloads,
packaged as Qt resource data. They are neither hardcoded nor inferred from a
host installer. The image's unknown version remains zero when a fallback is
chosen; the log identifies the inference and pending native compatibility.

File type, size and modification time are checked before and after the scan.
These checks detect ordinary removal/mutation, but do not pin image identity or
replace a later write-time check. Changes after completion need a rescan.
Individual reader calls remain synchronous: interruption happens at the shared
boundaries and teardown joins the worker, so a blocked read can delay shutdown.

Validation:

- **21/21 Linux CTest suites pass**, about 14.9 seconds, including existing
  image, reporting, Widgets/scaling and private-D-Bus tests.
- **21/21 pass with address, undefined-behavior and leak checks**, about 19.0
  seconds, without sanitizer diagnostics. All tests use ordinary-user temporary
  files and private sockets; no USB, mounting, root prompt or device writes.
- **15 new Qt functional cases** exercise real generated ISO9660 files through
  the file picker, concrete worker, shared C scanner and Widgets. Coverage
  includes complete/partial results; v4/v6 unknown-version fallback; UI-thread
  delivery; Cancel/Escape, log close/reopen and restart; rapid replacement;
  same-path reselection; invalid replacement; an already-exited worker's queued
  success; size/mtime change and removal; exception and overlap recovery; and
  controller/window teardown with queued reports.
- Link-time gates pause real scans before/after analysis to make lifecycle races
  reproducible. They use no production test hook, alternate worker factory or
  fake USB. The earlier file-picker test now expects a clear unsupported-image
  failure for its plain-text fixture instead of the old 'not analyzed' state.
- The analyzed-image offscreen screenshot was inspected. Captions, ordering,
  paired fields and advanced defaults still follow the Win32 form; no `.ui`
  changes were needed. Native desktop dialogs/themes, Windows rendering and
  real distribution booting remain unqualified.
- The first sanitizer build hit a GCC internal compiler error in the unchanged
  `grub_inspect_probe.c`; retrying the same build without source or flag changes
  succeeded. The completed build/test results above use those unchanged flags.
  `git diff --check` passes. Work is local and uncommitted; remote CI and native
  Windows validation remain unexecuted.

Reproduce this increment:

```sh
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build -R rufus_image_analysis_widgets --output-on-failure
/tmp/rufus-linux-qt6-build/rufus-linux
```

Use SELECT for a regular ISO9660 image; open the existing Log button for its
report and pending checks. See the [runtime contract](../linux/README.md#select-and-the-qt-analysis-job)
and [Win32 mapping](linux-win32-ui.md#cp3d-analysis-behavior).


## CP3e: shared UDF traversal and real image fixtures (2026-09-14)

**Status: implemented and verified locally.** SELECT now analyzes real UDF
filesystems through the same Qt job and shared file/bootloader stages. START
remains disabled; no media operations or privilege requests were introduced.

Changes:

- `src/iso_scan.c` contains `ScanUdfFiles` beside the ISO9660 walker. A common
  file-accounting function records paths, classification, block counts and
  Syslinux presence. UDF iterator ownership is explicit on success/error/cancel.
- Windows scan mode calls the shared UDF walker. Its mixed `check_iso_props`
  scan branch is removed; extraction remains in the Windows implementation.
  No new source modules, `#ifdef` layer or callback infrastructure were needed.
- The coordinator is renamed `AnalyzeIsoImage`. Its plain result identifies
  UDF/ISO9660 and whether a companion PVD provided a size check. No PVD means
  unknown volume-size comparison, not a verified zero mismatch.
- UDF-first routing is consistent with content reads: after a successful UDF
  open, a missing root fails instead of falling back to the ISO view. Unreadable
  child directories fail instead of being skipped. This is an intentional change
  to Windows scan behavior; extraction's fallback is retained. UDF scan paths
  now use the existing 260-byte shared limit and fail explicitly beyond it.
- The bundled native reader checks complete sector reads, allocation and
  directory-record bounds. It handles compressed 8-bit Unicode as UTF-8 and
  reads directory fields with their specified little-endian encoding.
- Qt identifies completed UDF scans in the existing status text. Volume label,
  pending checks, log, CLOSE/CANCEL, stale-result rejection and layout are shared.
  Unknown Syslinux versions remain unknown; native installer compatibility still
  requires its later gate.

Verification:

- All **22/22** normal Linux CTest suites pass; the updated UDF suite also passes
  with its additional 17 reader-contract checks. All **22/22** suites pass with
  ASan, UBSan and leak detection after those checks were added.
- Thirteen generated UDF files cover byte-exact contents, empty files/directories,
  mixed case and Unicode, multi-sector directories, nested trees, block accounting,
  Syslinux/GRUB/EFI content, inferred unknown versions, Windows/embedded-EFI pending
  reasons, path/config limits, cancellation and injected read/storage failures.
- A valid equal-length FID rename, with descriptor CRC/tag checksum repaired,
  creates differing UDF and ISO views. Tests verify the UDF tree and reject ISO
  fallback for missing UDF paths. A copy without ISO descriptors still scans UDF
  and reports its PVD size comparison unavailable.
- Seventeen checks use the real reader on a generated file with link-time short
  I/O/allocation failures and bounded directory inputs. These failure checks run
  only against the corrected code. The coordinator contract has 49 checks,
  including cleanup after UDF root failure without ISO fallback.
- Two added Qt cases (17 behavioral cases total) cover completed UDF metadata,
  pending Windows metadata, cancellation after scanning, cleared queued reports
  and restart. The Win32-mapped `.ui` form is unchanged.
- Both native **genisoimage 1.1.11** and **cdrtools mkisofs 3.02a09** pass the UDF
  fixtures. Generators were extracted under `/tmp/rufus-cp3e-tools`; no system
  packages were installed. The cdrtools archive matched the local distro package
  database SHA-256. CI now installs genisoimage as a required test dependency.
- Normal build: `/tmp/rufus-linux-qt6-build`; sanitizer build:
  `/tmp/rufus-linux-qt6-sanitizers.ssKskZ`. Ninja used its default parallelism.
  Tests use ordinary temporary files, offscreen Widgets and private D-Bus buses;
  no root, host devices, USB, mounts or Windows executables are involved.

Reproduce from the repository root (install genisoimage or mkisofs, or supply
its absolute path with `-DRUFUS_MKISOFS=...`):

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_udf_images|rufus_image_analysis_widgets|rufus_iso_analysis_contract' --output-on-failure
```

This is generated-layout metadata coverage. Other UDF revisions/allocation
layouts, multi-extent payloads, real distribution images, native Windows and
booting remain unqualified. Source changes are local and uncommitted; remote CI
has not run. Native Windows validation remains deferred as agreed.

## CP3f1: shared ISO9660 embedded EFI/FAT inspection (2026-09-14)

**Status: implemented and verified locally.** SELECT now inspects embedded FAT
images in the selected ISO9660 view using Rufus's existing libfat reader.
CP3f is split into two checkpoints because the original embedded-image transport
is ISO9660-only. UDF access remains CP3f2; no companion-ISO fallback was added to
the Linux operation. The Win32-mapped form is unchanged and START stays disabled.

Changes:

- `src/embedded_efi.c` separates the original EFI directory/name search from
  `iso.c`. Windows retains `HasEfiImgBootLoaders` as a small wrapper; Linux calls
  the same inspection from `AnalyzeIsoImage`. Windows extraction and its original
  FAT callback remain separate. Native Windows execution is still deferred.
- `InspectIsoEfiImage` borrows an ISO handle, accepts a candidate path and returns
  status plus a presence flag. It uses libfat's existing storage callback and the
  existing direct cancellation/logging hooks, with no Qt callback registry.
- The reader preserves 32 KiB read-ahead, bounded by the candidate file, and
  the original first-active-MBR FAT partition rule and ISO-alignment constraint.
  Partition and declared FAT sizes must fit the candidate. It saves/restores
  libfat's existing sector-size globals; the host still serializes operations.
- Original architecture names and RISC-V/LoongArch `~1` short-name aliases are
  retained. The aliases remain a heuristic, not a complete long-name resolver.
  FAT32 directory lookup now uses both cluster halves; directories and empty
  files named like bootloaders do not count as bootloader presence.
- The bundled reader releases its cached boot sector when opening fails, uses
  its portable bytewise little-endian helpers on every architecture, and bounds
  directory cache growth. Directory-chain cycles and invalid successor clusters
  return failure. These are focused reader changes, not a complete parser audit.
- Sector-size definitions now belong to libfat instead of the Windows installer.
  The native build supplies a small POSIX aligned-allocation adapter and builds
  the existing open/cache/directory/FAT-chain sources. Windows source manifests
  register the shared inspection module.

The result distinguishes completed absence, detected presence and incomplete
inspection. Completed absence clears the embedded-EFI pending reason. Presence
sets Rufus's existing `has_efi = 0x8000` marker and leaves Secure Boot pending.
Unreadable/unsupported candidates retain embedded-EFI pending; errors and
cancellation discard the result. This checks directory entries, not executable
contents, signatures or bootability. Existing ordinary-EFI/repair cases retain
their original skip conditions.

Verification:

- All **23/23 Linux CTest suites pass**, about 16.0 seconds; all **23/23 pass
  with ASan, UBSan and leak detection**, about 23.3 seconds, without sanitizer
  diagnostics. Ninja used its default parallelism; `git diff --check` passes.
- **27 generated container fixtures** cover FAT12/16/32, 512-byte and 4096-byte
  sectors, all eight architecture names, FAT32 directory clusters above 65535,
  final read-ahead at EOF, completed absence, MBR partition limits, truncated
  candidates and UDF deferral. Even the 34 MiB FAT32 case reads fewer than 128
  ISO blocks during embedded inspection; each read is at most 16 ISO blocks.
- Link-time failures cover each observed ISO read, reader/open/cache allocation,
  cancellation, restored sector-size globals and recovery. Invalid directory
  chain and open-failure fixtures run only against the corrected implementation.
- A new Qt functional case checks found/absent/unreadable outcomes through
  SELECT, the real worker and existing status/log/label controls, plus cancellation
  after inspection. There are now **18 Qt analysis behavioral cases**.
- Native `mkfs.fat`, `mcopy` and `mmd` create ordinary temporary files; no USB,
  mounts, root prompts or device writes are involved. CMake requires these test
  tools and the Linux workflow now includes `dosfstools` and `mtools`.

An El Torito catalog's load count may describe only the initial boot load, not
the whole embedded FAT image. A generated hard-disk entry exposing one sector
therefore stays pending; an explicit complete no-emulation range is readable.
Do not extend a candidate range merely because its FAT header claims more bytes.
Real distribution layouts and booting still need separate qualification.

Reproduce from the repository root, with the native test tools available:

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /tmp/rufus-linux-qt6-sanitizers.ssKskZ --output-on-failure
```

The sanitizer build uses `-fsanitize=address,undefined -fno-omit-frame-pointer`
for compilation and sanitizer linker flags. Tests require ordinary-user local
socket access for private D-Bus fixtures. Work remains local and uncommitted;
remote CI and native Windows validation have not run.

## CP3f2: bounded UDF access to embedded FAT (2026-09-14)

**Status: implemented and verified locally.** Embedded EFI inspection now reads
through the selected UDF filesystem, reusing CP3f1's cache, FAT preparation and
directory search. No host mounts, whole-image buffers or new GUI callbacks are
involved. The Win32-mapped form is unchanged and START stays disabled.

Changes:

- The shared entry point is now `InspectEfiImage(iso, udf, path, found)`. Exactly
  one filesystem handle must be supplied; missing/unreadable UDF data never
  triggers an ISO fallback. The caller owns and serializes the handle. UDF access
  changes its file position; later users must seek or open their file normally.
- The UDF adapter uses the bundled `udf_setpos` and `udf_read_block` APIs. A
  cache fill follows short reads into subsequent extents with explicit seeks.
  It retains the 32 KiB cache and checks cancellation between reads.
- The existing UDF offset translator now returns the bytes remaining in an
  extent after a seek, validates descriptor storage bounds and decodes fields
  as little-endian. Block reads stop at extent/file boundaries and reject
  unrepresentable positions. API documentation now correctly distinguishes the
  block-count argument from the returned byte count.
- Recorded short/long descriptors in a file entry and the first partition map
  are supported. Sparse, indirect/extended and in-ICB allocations remain
  unsupported; encountered unsupported data leaves inspection pending. This is
  a focused streaming correction, not a complete UDF parser audit.
- Both the Linux coordinator and Windows `HasEfiImgBootLoaders` wrapper select
  the UDF view when present. This intentionally changes Windows's former use
  of the companion ISO for embedded inspection. Its extraction implementation
  remains separate; native Windows validation is still deferred as agreed.
- Found EFI leaves Secure Boot pending, completed absence clears the embedded
  check, and unreadable candidates remain incomplete. The existing Qt test now
  exercises those outcomes and cancellation for both filesystem views.

Verification:

- **24/24 normal CTest suites pass**, about 17.8 seconds. **24/24 pass with ASan,
  UBSan and leak detection**, about 26.3 seconds, without sanitizer diagnostics.
  Qt analysis has 19 behavioral cases/data rows (21 including setup/cleanup).
- The new `rufus_udf_embedded_efi` suite creates **16 ordinary UDF containers**:
  FAT12/16/32, 4 KiB sectors, high FAT32 directory clusters, EOF read-ahead,
  absent/unreadable/truncated candidates, MBR wrapping, distinct UDF/ISO views,
  Unicode/case-sensitive paths and UDF without a companion ISO.
- Two fixtures reorder allocated file blocks and describe three non-contiguous
  extents using short and long allocation descriptors. **22 direct range
  checks** verify exact returned bytes, seeks inside extents, EOF, final padding,
  unaligned/out-of-range seeks and injected short physical reads.
- Link-time seek/read/root/file/storage failures and cancellation check cleared
  presence, retained pending reasons and restored FAT sector settings. Thirteen
  descriptor-bound/unsupported-layout checks run only on the corrected reader.
  The existing ISO9660 fixtures and all other suites continue to pass.
- Each embedded data-read request is at most 16 optical blocks. The large FAT32
  fixture still requests fewer than 128 blocks during inspection. Tests use
  native fixture tools, temporary files, offscreen Widgets and private D-Bus
  sockets; no root, USB devices, mounts or media writes are required.
- Both genisoimage 1.1.11 and cdrtools mkisofs 3.02a09 pass the new suite normally
  and with sanitizers. The fixture builder preserves mkisofs's extended
  attributes before rewriting allocation descriptors; this also exercises
  nonzero descriptor offsets. No production changes were needed for that variant.

Reproduce from the repository root:

```sh
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure
cmake --build /tmp/rufus-linux-qt6-sanitizers.ssKskZ
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /tmp/rufus-linux-qt6-sanitizers.ssKskZ --output-on-failure
```

Ninja uses its default parallelism. The sanitizer flags are unchanged from
CP3f1. `git diff --check` passes. Work remains local and uncommitted; native
Windows and remote CI have not run. The older whole-file UDF buffer reader still
performs a single read and does not assemble multi-extent payloads. Embedded
streaming coverage does not qualify all UDF layouts or real distribution booting.
El Torito load counts with unknown full extents remain pending; Secure Boot and
native installer compatibility remain separate gates.

## CP3g1: shared WinPE options and WIM boundary (2026-09-14)

**Status: implemented and verified locally.** WinPE `txtsetup.sif` inspection
now runs through the shared scanner and Qt job. WIM metadata remains a separate
checkpoint. See the [Windows-image metadata source map](linux-windows-image-metadata.md)
for the distinction between WIM archive-format version, Windows release/build,
edition selection and embedded WIM access.

Changes:

- `src/winpe_scan.c` contains the scan formerly mixed into `ExtractISO`, using
  the SIF-relevant line grammar from `parser.c`. Both Windows and Linux call it
  at the same position in their scan. The general parser/update-data consumers
  remain unchanged; no wide-character or GUI adapter was introduced.
- Original WinPE masks, i386/AMD64/minint directory priority, first nonempty
  option value and `/minint` substring matching are retained. Classification
  records the original-case SIF paths for case-sensitive UDF access.
- `ReadISOFileToBufferLimited` checks the caller's limit before allocating or
  reading file data. WinPE uses 64 KiB; the existing API retains its 1 GiB limit.
  Both entry points use the same UDF-first lookup and ownership rules.
- Ordinary text no longer requires a NUL byte at the end of the on-disc file.
  A single trailing NUL is accepted and a UTF-8 BOM is skipped. Empty/oversized
  files, embedded NULs and UTF-16 remain explicitly unavailable. The parser is
  scoped to SIF lines, without the general parser's RTF brace grouping.
- `winpe_options_known` separates inspected absence from unavailable options.
  WinPE-only success clears the Windows pending reason; a boot manager or WIM
  still retains its separate metadata check. Failure/cancellation clears the
  options. The existing Qt status, label and log show the result; the Win32-mapped
  form is unchanged and START stays disabled.

Verification:

- **26/26 normal CTest suites pass**, about 19.1 seconds; **26/26 pass with
  ASan, UBSan and leak detection**, about 28.3 seconds, without diagnostics.
- The options component passes **171 assertions** across 42 text/boundary
  inputs plus argument/storage checks. Twenty generated ISO/UDF containers test
  directory priority, original-case lookup, distinct UDF/ISO views, known absence,
  unsupported/oversized content, mixed WinPE/WIM pending checks and cancellation.
- Six additional reader-contract scenarios prove the 64 KiB inclusive bound,
  rejection before allocation/data I/O and invalid limits on both backends;
  that suite now has 30 scenarios plus argument checks.
- The new Qt functional case runs for ISO9660 and UDF through the real worker,
  covering READY/partial results, cleared stale options and cancellation after
  scanning. Qt analysis now has **21 cases/data rows** (23 with setup/cleanup).
- The generated-image suite passes with both genisoimage and cdrtools mkisofs.
  Fixtures are ordinary files; no Windows executables, root, USB or mounts are
  needed. Windows source manifests include the new module; native Windows and
  remote CI remain unexecuted. Work is local and uncommitted.

Reproduce from the repository root:

```sh
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure
cmake --build /tmp/rufus-linux-qt6-sanitizers.ssKskZ
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /tmp/rufus-linux-qt6-sanitizers.ssKskZ --output-on-failure
```

Ninja uses its default parallelism. `git diff --check` passes. These checks
qualify metadata and existing Widgets behavior; booting and installation remain
unqualified. Unsupported SIF encodings and pending WIM/Secure Boot stages are
not silently treated as completed checks.

**Next: CP3g2.** Establish native wimlib metadata queries on generated ordinary
WIM files, sharing Rufus's version interpretation. Follow with CP3g3 for WIMs
inside UDF/ISO9660, assessing reuse of Rufus's existing transport before choosing
an alternative. No default whole-install-image copy into RAM or temporary files.
Privilege handling, media creation, Secure Boot, raw/hybrid analysis and boot
validation retain their separate checkpoints.

## CP3g2: native WIM metadata (2026-09-14)

**Status: implemented and verified locally.** Ordinary WIM files can now be
queried through native wimlib. This establishes the metadata component and shared
version interpretation; ISO/UDF transport and integration with SELECT remain
CP3g3. The [metadata contract](linux-windows-image-metadata.md) records ownership,
unknown values, version rules and cancellation limits.

Changes:

- `src/wim_metadata.c` shares the archive query from `vhd.c` and normalization
  from `wue.c`. Windows retains its existing opening/XML conversion and calls
  these shared functions. Source manifests include the module; CRLF/BOM formats
  are preserved. No Windows runtime or broad compatibility layer was added.
- `ReadWimMetadata` uses wimlib's public property API for MAJOR/MINOR/BUILD/SPBUILD.
  Fixed ASCII names and decimal parsing work with the library's native/Windows
  character types. The result separates archive version/count/index/part facts,
  raw version values, normalized Windows version and missing/invalid fields.
- `linux/platform/wim_file.c` handles ordinary-file opening and direct host
  cancellation. A held read-only descriptor prevents pathname replacement from
  switching the selected file. Directories/FIFOs fail without waiting. Errors
  and cancellation clear metadata, release handles and permit subsequent scans.
- CMake uses native wimlib headers and binaries, with rootless prefix support.
  The Ubuntu workflow now declares `libwim-dev`. A fresh reporting-only configure
  and build succeeds without this dependency. Qt's Win32-mapped form is unchanged.

Verification:

- **28/28 normal suites pass**, 18.61 seconds. **28/28 pass with ASan, UBSan and
  leak detection**, 28.62 seconds, without diagnostics. Existing private-D-Bus
  and offscreen Widgets tests remain included.
- The metadata suite checks **94 generated WIM/file fixtures and 647 assertions**:
  normal/solid archive formats, multiple and zero images, boot-index versus first
  image selection, Windows mapping boundaries, raw versus normalized version,
  UTF-8/space-containing paths, numeric bounds, absent and invalid fields,
  truncated files, missing files, directories and FIFOs.
- **1,849 ownership/error/cancellation checks** cover each cancellation boundary,
  injected open/info failures with retained library codes, invalid arguments,
  empty properties, repeated failure/cancel/success cycles, stable descriptor
  counts and replacement of the original pathname during opening.
- Tests create and read their own tiny WIMs using the library API. They need no
  USB, root, mounts, external WIM downloads or Windows executables. The native
  distro library itself is not sanitizer-instrumented; our modules and tests are.
  The package allocations remain visible to leak detection.

Native dependency used locally, extracted under `/tmp/rufus-cp3g2-wimlib` from
the Arch mirror `https://geo.mirror.pkgbuild.com/extra/os/x86_64/`. SHA-256 values
were checked against this host's `/var/lib/pacman/sync/extra.db` before extraction:

| Package | SHA-256 |
| --- | --- |
| `wimlib-1.14.5-3-x86_64.pkg.tar.zst` | `59a6b6bd33ec5bd355b53dbe3e649ef8767c2d4f931a1bf356b93b747c19940e` |
| `libntfs-3g-2026.7.7-1-x86_64.pkg.tar.zst` | `3fa50fc17a605d806a3ea8889dbe972f2f2ccc647ac56a24a2b4e255877ebb34` |

No system packages or library search configuration were changed. The remaining
runtime dependencies already existed on the host. Tests do not invoke NTFS or
FUSE operations. Distro versions differ; CI on Ubuntu is prepared but unexecuted.

Reproduce with the extracted dependency (or omit the prefix/environment when
native wimlib and its dependencies are installed normally):

```sh
export LD_LIBRARY_PATH=/tmp/rufus-cp3g2-wimlib/usr/lib
cmake -S linux -B /tmp/rufus-linux-qt6-build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/tmp/rufus-cp3g2-wimlib/usr
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure
cmake -S linux -B /tmp/rufus-linux-qt6-sanitizers.ssKskZ -DCMAKE_PREFIX_PATH=/tmp/rufus-cp3g2-wimlib/usr
cmake --build /tmp/rufus-linux-qt6-sanitizers.ssKskZ
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /tmp/rufus-linux-qt6-sanitizers.ssKskZ --output-on-failure
```

The existing sanitizer build retains `-fsanitize=address,undefined
-fno-omit-frame-pointer` on C/C++ and matching sanitizer linker flags. Ninja uses
default parallelism. `git diff --check` passes. Native Windows, remote CI and
boot/media-creation tests remain deferred; work is local and uncommitted.

**Next: CP3g3.** Assess reuse of Rufus's bundled libcdio WIM transport with native
wimlib, covering selected ISO/UDF view, bounded reads and cancellation before
SELECT integration. Do not silently buffer or copy an entire install WIM as a
default workaround. Native wimlib calls are currently synchronous; cancellation
is checked around them, and the library's internal allocation/I/O has no new
global resource bound. WIM extraction/splitting, edition selection, Secure Boot,
privileged media operations and real boot qualification retain later checkpoints.

## CP3g3a: shared ISO/UDF file ranges (2026-09-14)

**Status: implemented and verified locally.** The byte reader previously inside
embedded EFI inspection is now a shared concrete operation in `src/iso_file.c`.
EFI uses it immediately, and generated WIMs exercise the candidate input for the
next native wimlib transport step. This checkpoint does not yet pass the reader
to native wimlib or connect WIM-in-container metadata to SELECT.

The [source map and transport decision](linux-windows-image-metadata.md#cp3g3a-shared-file-ranges-and-the-native-library-boundary)
records why the system library cannot accept this reader through its public API,
and why a native prototype retaining Rufus's existing extension is the preferred
next step. CP3g3 is split into this reader checkpoint, CP3g3b for native build and
transport qualification, and CP3g3c for SELECT integration.

Changes:

- `OpenIsoFileReader`, `ReadIsoFileRange` and `CloseIsoFileReader` borrow exactly
  one selected ISO/UDF handle and retain the existing 32 KiB cache. The UDF file
  entry is reader-owned. No cross-view fallback or per-read allocation occurs.
- Arbitrary byte ranges can cross block/cache boundaries, seek backward and
  traverse fragmented UDF extents. ISO multi-extents keep the existing bundled
  reader's contiguous-extent requirement. Bounds are checked before I/O; every
  backend call is at most sixteen blocks, with padding excluded from output.
- Cancellation applies to cache hits and refill boundaries. Errors invalidate
  incomplete refills; callers discard the requested output. Existing EFI/libfat
  policy, global-sector restoration and Widgets behavior remain covered.
- Empty files support a zero-byte EOF read. Testing with cdrtools exposed its
  non-data extent sentinel for empty ISO files; the opener now requires a usable
  sector address only for nonempty files. Positive reads from empty files fail
  before I/O. No bundled libcdio change was needed for this adjustment.
- Windows source manifests include the new shared module, retaining file formats.
  The native dependency remains the CP3g2 library; no new package was installed.

Verification:

- **30/30 normal CTest suites pass**, 18.88 seconds. **30/30 pass with ASan,
  UBSan and leak detection**, 31.62 seconds, without sanitizer diagnostics.
- **380 direct contract checks** cover cache reuse/refills, forward/backward and
  unaligned reads, zero-length/EOF behavior, overflow/range rejection, failure,
  short reads, cancellation and recovery. Offsets above 4 GiB use a deterministic
  backend test double; this is not a multi-gigabyte real-image qualification.
- **102 functional probe runs compare 814 ranges and 9,451,034 bytes**. Two native
  generated WIMs (81,424 and 81,418 bytes) and ordinary data/empty/UTF-8 files are
  placed in three generated ISO/UDF containers. Checks cover RR/Joliet/UDF views,
  different WIMs at the same path across views, missing UDF paths without ISO
  fallback, and fragmented short/long UDF allocation descriptors.
- Four scattered small reads within one cached region need one 32 KiB refill.
  Refills remain at most sixteen blocks, including fragmented files. These are
  transport measurements; no claim is made about wimlib's internal allocation or
  embedded metadata performance before CP3g3b.
- Both genisoimage and cdrtools mkisofs pass the new functional suite, normally
  and under sanitizers. Existing generated FAT12/16/32, ISO/UDF, private-D-Bus and
  offscreen Widgets suites also pass. No USB, root, mounts or Windows executables
  are needed. Native Windows, remote CI, extraction and boot tests remain deferred.

Reproduce with CP3g2's extracted dependency (omit the environment override when
native wimlib is installed normally):

```sh
export LD_LIBRARY_PATH=/tmp/rufus-cp3g2-wimlib/usr/lib
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure
cmake --build /tmp/rufus-linux-qt6-sanitizers.ssKskZ
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /tmp/rufus-linux-qt6-sanitizers.ssKskZ --output-on-failure
python3 linux/tests/test_iso_file.py /tmp/rufus-linux-qt6-build/iso_file_probe /tmp/rufus-linux-qt6-build/wim_fixture --mkisofs /tmp/rufus-cp3e-tools/usr/bin/mkisofs
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 python3 linux/tests/test_iso_file.py /tmp/rufus-linux-qt6-sanitizers.ssKskZ/iso_file_probe /tmp/rufus-linux-qt6-sanitizers.ssKskZ/wim_fixture --mkisofs /tmp/rufus-cp3e-tools/usr/bin/mkisofs
```

Ninja uses its default parallelism. Existing bundled-header pedantic warnings
remain; new component/test code builds without its own diagnostics. Build logs
are `/tmp/rufus-cp3g3a-build.log` and `/tmp/rufus-cp3g3a-sanitizers-build.log`;
CTest logs remain in each build's `Testing/Temporary/LastTest.log`.
`git diff --check` and source-manifest XML/format checks pass. Work remains local
and uncommitted.

**Next: CP3g3b.** Prototype the native build of Rufus's existing wimlib input
extension, using matching upstream Unix/build support and the shared reader.
Compare build/source maintenance cost and ordinary-versus-embedded metadata;
test selected views, sequential/random position, errors/cancellation and resource
cleanup before selecting the application dependency arrangement. Avoid whole-WIM
staging, mounts, another WIM/XML parser or process-wide POSIX interception.

## CP3g3b: upstream wimlib reader extension (2026-09-14)

**Status: implemented and verified locally as an opt-in target.** During the
source comparison, the user chose upstream wimlib plus a small reader extension
instead of porting the bundled Windows fork. A separate upstream-based fork is
an option for later maintenance. The [prototype report](linux-wimlib-prototype.md)
records the comparison, exact source pin, build commands and next boundary.

The extension uses the upstream commit recorded by Rufus's bundled version,
with native POSIX/Unix support. Five upstream files receive 44 added and 5
removed lines; a 76-line adapter supplies the existing shared ISO/UDF ranges.
WIM format/XML parsing stays upstream. The system 1.14.5 library remains an
independent ordinary-file comparator, never linked into a prototype process.
CMake verifies 137 source/header inputs and patches build copies with zero fuzz.
It does not download dependencies or modify the Windows wimlib tree.

Verification:

- **33/33 full normal CTest suites pass** in 19.17 seconds.
- **33/33 pass with ASan, UBSan and leak detection** in 33.07 seconds, without
  sanitizer diagnostics. The new upstream sources are instrumented too.
- The upstream build passes the existing **94 WIM/file fixtures and 647 metadata
  checks**. **132 direct reader checks** cover position, random/backward reads,
  >4 GiB offset arithmetic through a test double, EOF, range rejection, read-only
  behavior, cancellation, failures and repeated failed-open cleanup.
- **95 functional probes** compare ordinary and embedded metadata, including
  normal/pipable/solid/integrity WIMs, UTF-8 paths, invalid/truncated/empty input,
  explicit RR/Joliet/UDF selection, different WIMs at one path, missing paths
  without fallback and fragmented short/long UDF allocation descriptors.
- Read failures and zero-byte short reads are injected at each observed cache
  refill for the normal generated WIM. Cancellation is tested before opening
  and at every observed refill boundary. Borrowed readers remain usable after
  freeing successful or failed WIM opens. Sanitizers cover fixture cleanup.
- Both genisoimage and cdrtools mkisofs pass, normally and with sanitizers.
  A changed upstream header is rejected during configuration. A fresh
  reporting-only build still succeeds without wimlib or the upstream source.

The fixture WIMs contain generated metadata, not Windows installation payloads.
No root access, USB, mounts or Windows binaries are used. Internal wimlib
allocations/CPU work have no new global bound; synchronous I/O is cancellable
between calls. Exports, extraction, retained resources, real media, actual boot,
Windows execution and remote CI remain unqualified.

Build logs: `/tmp/rufus-cp3g3b-build.log` and
`/tmp/rufus-cp3g3b-sanitizers-build.log`; full test logs remain under each build's
`Testing/Temporary/LastTest.log`. Ninja used default parallelism. Existing upstream
and bundled-header compiler warnings remain; the new adapter/test code has no
diagnostics of its own. Changes remain local and uncommitted.

**Next: CP3g3c.** Promote one extended upstream provider for the application and
connect Windows metadata to the existing SELECT job, with explicit pending,
known, failed and cancelled states plus offscreen Widgets tests. Preserve the
Win32 form. Keep the reader extension separately reviewable when qualifying a
supported upstream release; do not make maintaining Rufus's Windows fork a Linux
requirement.

## CP3g3c: SELECT Windows metadata (2026-09-14)

**Status: implemented and verified locally.** SELECT now obtains installation
WIM archive facts and Windows version fields through the qualified upstream
reader extension. The existing Win32-mapped form, controls and layout are
unchanged; results appear in its current status and log.

The build uses one extended upstream wimlib for the application and ordinary-file
components. `RUFUS_WIMLIB_UPSTREAM` identifies the pinned source tree, with the
existing verified manifest and patch. `RUFUS_COMPARE_SYSTEM_WIMLIB=ON` optionally
builds separate system-library comparison executables. The previous prototype
option is removed. No configuration-time downloads, Windows compatibility aliases
or changes to the upstream input patch were needed for this integration.

`AnalyzeIsoImageWithSource` retains exactly the selected filesystem handle,
closing any companion ISO used for UDF size checks. The caller owns the handle
until `CloseIsoAnalysisSource`; error/cancellation leaves it closed. The legacy
`AnalyzeIsoImage` wrapper preserves close-on-return behavior. The shared scanner
gains no WIM or Qt dependency. The Windows stage borrows this handle and uses
the first recorded installation path and WIM image 1, preserving Rufus's existing
automatic reporting policy. It neither reopens the container nor retries a later
candidate after failure or unknown version fields.

`WINDOWS_IMAGE_ANALYSIS` travels with the existing request number. It records
unchecked/known/unknown/failed state, metadata validity masks and library errors.
Successful facts populate the existing report's archive and Windows-version
fields. Unknown or unreadable metadata keeps the useful tree result partial;
missing revision stays unknown rather than becoming a reported zero. A known WIM
cannot clear unknown WinPE options, Secure Boot or independent pending checks.
Boot-manager-only images without an installation WIM retain their Windows check.
Cancellation clears both outputs and releases WIM/reader ownership. Replacement
selections cannot publish obsolete facts or version logs.

Verification:

- **34/34 full CTest suites pass normally**, 22.44 seconds.
- **34/34 pass with ASan, UBSan and leak detection**, 37.85 seconds, without
  sanitizer diagnostics. This includes the application provider and native source.
- **22 new native integration probes** cover generated known/missing/invalid/
  incomplete version fields, multiple WIM images with boot index 2, first-path
  failure/unknown policy, original-case paths, selected UDF with different
  companion-ISO contents, metadata in `.esd`/`.swm` named files, cancellation and
  pending-reason merging. The suffix cases contain ordinary generated WIMs;
  they do not qualify split-part extraction or encrypted ESD payloads.
- **15 new Widgets cases/data rows** test normal and UDF installation images,
  known/unknown/failed states, archive versus Windows version display, image 1
  rather than the boot index, missing revision, selection reset, cancellation
  before WIM opening and after metadata, and replacement of a completed WIM
  result. The image-analysis suite now has **38 passing cases**, including its
  setup/cleanup. Real mouse/keyboard cancellation and worker-thread delivery are
  retained; START remains disabled.
- Existing WIM metadata, reader bounds/cleanup, EFI/FAT, file classification,
  Syslinux, private-D-Bus and Win32 layout tests also pass. The new native suite
  passes with both genisoimage and cdrtools mkisofs.
- A fresh **application-only build without system wimlib** succeeds and its
  linked libraries resolve without `libwim.so`. The reporting-only build still
  succeeds without a WIM source/dependency. Upstream source hashes are unchanged.
- The Ubuntu workflow now explicitly downloads/verifies the pinned archive and
  enables the independent system comparator. It is prepared but not remotely run.

Reproduce with the source obtained as described in the
[upstream reader document](linux-wimlib-prototype.md#reproduction):

```sh
export LD_LIBRARY_PATH=/tmp/rufus-cp3g2-wimlib/usr/lib
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_WIMLIB_UPSTREAM=/tmp/rufus-cp3g3b-upstream -DRUFUS_COMPARE_SYSTEM_WIMLIB=ON -DCMAKE_PREFIX_PATH=/tmp/rufus-cp3g2-wimlib/usr
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure
cmake -S linux -B /tmp/rufus-linux-qt6-sanitizers.ssKskZ -DRUFUS_COMPARE_SYSTEM_WIMLIB=ON
cmake --build /tmp/rufus-linux-qt6-sanitizers.ssKskZ
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /tmp/rufus-linux-qt6-sanitizers.ssKskZ --output-on-failure
```

The existing sanitizer build retains its ASan/UBSan compiler and linker flags.
Without comparison tests, omit the system prefix and runtime override; a fresh
GUI build needs only `-DBUILD_TESTING=OFF` and `-DRUFUS_WIMLIB_UPSTREAM=...`.
Normal and sanitizer build logs are `/tmp/rufus-cp3g3c-build.log` and
`/tmp/rufus-cp3g3c-sanitizers-build.log`; full test logs remain in each build's
`Testing/Temporary/LastTest.log`. The fresh application build is
`/tmp/rufus-cp3g3c-app`. Ninja uses default parallelism.

This checkpoint uses generated metadata and no USB, root, mounts or Windows
execution. It does not qualify WIM extraction/export lifetimes, Secure Boot,
media writing or actual booting. Library allocations/CPU work are not globally
bounded; cancellation remains cooperative. Windows validation and remote CI
remain deferred. Changes remain local and uncommitted.

**Next checkpoint:** qualify the reader extension against a supported upstream
release before expanding WIM operations. Keep raw/hybrid detection, WIM-contained
EFI inspection, Secure Boot and native media installers as separate testable
steps; metadata success does not complete those decisions.

## CP3g3d: wimlib 1.14.5 release (2026-09-14)

**Status: implemented and verified locally.** The Linux application now builds
from the official upstream **1.14.5 release**, replacing the CP3g3b comparison
commit. Upstream listed it as current when checked on 2026-09-14. The
[release announcement](https://wimlib.net/forums/viewtopic.php?t=1133) publishes
the source archive checksum; the downloaded archive matches it:
`84221a3abd5b91228f15f8e6065c335a336237b5738197b75bf419eea561a194`.

The same **five-file patch, 44 added and 5 removed lines**, applies with zero
fuzz and no offsets. `native_input.c` and its interface are unchanged. Of the
137 consumed upstream inputs, 59 changed since the comparison pin; 56 have only
license-header changes. The other changes concern an inode-capture warning, the
public version number and a Windows stat alias. The upstream configuration and
the WIM opening, I/O and XML interfaces we extend need no adaptation. The source
list remains 64 C files and 73 headers.

The SHA-256 manifest now verifies these release inputs. The native configuration
reports `1.14.5-rufus-reader`; two additional checks in the existing reader
contract verify the actual numeric and string versions. The workflow and current
build instructions use the official release archive and published checksum.
No dependency download occurs during CMake configuration. The source tree is
unchanged; only build copies receive the extension. A separate fork is still
optional and was not needed for this upgrade.

Verification:

- **34/34 full CTest suites pass normally**, 22.20 seconds.
- **34/34 pass with ASan, UBSan and leak detection**, 37.93 seconds, without
  sanitizer diagnostics. The upgraded upstream sources are instrumented.
- **134 direct reader checks** pass, including version identity, range/offset
  behavior, read-only input, failure, cancellation and borrowed-reader cleanup.
- The existing **94 WIM/file fixtures and 647 metadata checks** pass with both
  the extended provider and independent system library. **95 reader probes**,
  **22 Windows integration probes** and all **38 image-analysis Widgets cases**
  pass. Both genisoimage and cdrtools mkisofs pass the two WIM image suites;
  the full sanitizer run uses genisoimage.
- Configuration rejects the old comparison tree and a release copy with an
  altered `include/wimlib/file_io.h`. All 137 external source hashes remain
  unchanged after the builds. The exact reader patch and source list are also
  unchanged from CP3g3c.
- A fresh **Release application-only build** succeeds with no system-wimlib
  prefix/comparator. Its library dependencies resolve without `libwim.so`.
- `git diff --check` passes. Existing bundled/upstream compiler warnings remain.
  The Ubuntu workflow is updated but has not run remotely.

Reproduce after downloading the source using the
[release build instructions](linux-wimlib-prototype.md#reproduction):

```sh
export LD_LIBRARY_PATH=/tmp/rufus-cp3g2-wimlib/usr/lib
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_WIMLIB_UPSTREAM=/tmp/rufus-cp3g3d-upstream/wimlib-1.14.5 -DRUFUS_COMPARE_SYSTEM_WIMLIB=ON -DCMAKE_PREFIX_PATH=/tmp/rufus-cp3g2-wimlib/usr
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure
cmake -S linux -B /tmp/rufus-linux-qt6-sanitizers.ssKskZ -DRUFUS_WIMLIB_UPSTREAM=/tmp/rufus-cp3g3d-upstream/wimlib-1.14.5 -DRUFUS_COMPARE_SYSTEM_WIMLIB=ON
cmake --build /tmp/rufus-linux-qt6-sanitizers.ssKskZ
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /tmp/rufus-linux-qt6-sanitizers.ssKskZ --output-on-failure
```

The existing sanitizer build retains its ASan/UBSan compiler and linker flags.
The fresh application-only check uses:

```sh
cmake -S linux -B /tmp/rufus-cp3g3d-app -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DRUFUS_WIMLIB_UPSTREAM=/tmp/rufus-cp3g3d-upstream/wimlib-1.14.5
cmake --build /tmp/rufus-cp3g3d-app
```

Build logs are `/tmp/rufus-cp3g3d-build.log`,
`/tmp/rufus-cp3g3d-sanitizers-build.log` and
`/tmp/rufus-cp3g3d-app-build.log`. Source rejection checks are recorded in
`/tmp/rufus-cp3g3d-pin-tests.log`; full normal/sanitizer logs are in the existing
builds' `Testing/Temporary/LastTest.log`. Ninja uses default parallelism.

This qualifies the release for the currently implemented metadata/reader path.
It uses generated files, no USB, mounts or privileged media operations. WIM
extraction/export lifetimes, Secure Boot, media writing and actual boot are not
qualified. Windows validation and remote CI remain deferred. Changes are local
and uncommitted; the Widgets form is unchanged.

**Next checkpoint:** implement bounded native extraction of one EFI file from a
generated WIM and share Rufus's PE architecture inspection. Establish file-size
limits, resource ownership, errors and cancellation through ordinary and selected
ISO/UDF inputs before connecting it to SELECT. Keep Secure Boot, raw/hybrid
detection and native installers as independent later checkpoints.

## CP3g4a: single-file WIM extraction and PE architecture (2026-09-14)

**Status: implemented and verified locally as a native component.**
`ReadWimFile` queries one selected WIM image/path through public wimlib APIs,
rejects unsupported entries and outputs larger than **16 MiB**, extracts into a
private directory, checks the actual file length/type, and returns caller-owned
bytes. The temporary file and directory are cleaned up before return. Errors
and cancellation clear both outputs. The library's existing progress callback
and Rufus's direct cancellation query cover extraction and output reads. The
caller exclusively borrows the WIM; no exported resource references escape.

The upstream **1.14.5 reader patch is unchanged**. Tests now qualify its existing
I/O path for bounded one-file extraction as well as metadata. The function uses
ordinary native APIs, no Windows aliases, mounts, root access, stdout redirection
or whole-WIM staging. Its implementation and limitations are described in the
[Windows-image boundary](linux-windows-image-metadata.md#cp3g4a-bounded-single-file-extraction-and-shared-pe-recognition).

`src/pe_arch.c` separates Rufus's existing machine mapping and PE recognition
from Windows file mapping. `FindPeArch` requires bounded DOS, PE and COFF fields
and preserves the original architecture list. Unknown and truncated input return
`ARCH_UNKNOWN`; this is not executable, EFI subsystem or signature validation.
The Windows wrapper obtains the file size and calls this shared parser. Its
native mapping remains Windows-specific. Windows build manifests and the
standalone component workflow include the new source; Windows execution is
still deferred as requested.

Verification:

- **36/36 full CTest suites pass normally**, 27.61 seconds.
- **36/36 pass with ASan, UBSan and leak detection**, 58.87 seconds, without
  sanitizer diagnostics. The new code and upstream library are instrumented.
- **146 functional extraction probes** compare exact generated file bytes,
  covering uncompressed, LZX, XPRESS, LZMS solid, pipable and integrity WIMs.
  Image 1 and image 2 differ, with boot index 2. Empty files, UTF-8 paths,
  missing paths, directories, symlinks, unknown PE bytes, invalid image indices
  and case-sensitive path lookup are exercised.
- Ordinary files and RR/Joliet/UDF containers agree. Different WIMs at the same
  path across views remain distinct; a missing selected-UDF file does not fall
  back to ISO. Fragmented short/long UDF extents extract correctly. Physical
  reads remain at most sixteen blocks. A damaged file resource fails upstream's
  SHA-1 check, both ordinarily and inside UDF.
- Every observed refill in the uncompressed embedded fixture is exercised with
  read failure, short read and cancellation. Additional cases cover cancellation
  before, during and after extraction and during output reads, directory creation
  failure, extraction error, output read failure/short read, allocation failure
  and an output truncated between extraction and opening. A retry on the same
  WIM succeeds after each injected fault. Outputs remain empty on failure.
- Every probe verifies temporary-directory removal, descriptor counts and no
  retained progress callback. Borrowed readers remain usable after freeing the
  WIM. The exact 16 MiB limit succeeds; one byte above fails before extraction,
  including ISO/UDF inputs. Sanitizers verify allocation/resource cleanup.
- **289 portable PE checks** cover all original mappings, unknown machines,
  unaligned input, all truncation lengths, DOS/PE signature failures and
  overlapping/negative/oversized offsets. The input is not modified.
- Both genisoimage and cdrtools mkisofs pass all 146 extraction probes. The full
  sanitizer run uses genisoimage. Existing metadata, transport, Widgets, private
  D-Bus and Win32-layout tests continue to pass. The application also builds.

Reproduce with the existing upstream source and builds from CP3g3d:

```sh
export LD_LIBRARY_PATH=/tmp/rufus-cp3g2-wimlib/usr/lib
cmake -S linux -B /tmp/rufus-linux-qt6-build
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure
cmake -S linux -B /tmp/rufus-linux-qt6-sanitizers.ssKskZ
cmake --build /tmp/rufus-linux-qt6-sanitizers.ssKskZ
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /tmp/rufus-linux-qt6-sanitizers.ssKskZ --output-on-failure
```

Select the new tests with `-R 'rufus_wim_extract_images|rufus_pe_arch'`.
The PE component can also build independently from `tests/pe_arch` without Qt,
wimlib or libcdio. Build logs are `/tmp/rufus-cp3g4a-build.log` and
`/tmp/rufus-cp3g4a-sanitizers-build.log`; full test logs are in each build's
`Testing/Temporary/LastTest.log`. Ninja uses default parallelism.

The file-output limit does not bound all upstream metadata allocations or solid
resource decompression. Cancellation remains cooperative. Real Windows/EFI
payloads, split/delta-resource resolution, encrypted extraction, retained exports,
Secure Boot, media writing and boot tests are not qualified. Remote CI and native
Windows validation remain deferred. Changes are local and uncommitted.

**Next checkpoint:** qualify standalone WIM/ESD classification and selected
ordinary-file ownership before adding its EFI architecture to SELECT. Rufus's
original `ImageScanThread` applies image-1 `Windows/Boot/EFI/bootmgr.efi` to
`is_windows_img`; it does not use that file inside an ISO installation WIM to
infer firmware bootability of the ISO. Test Windows-compatible WIM path lookup,
unknown/missing/unreadable states, cancellation and replacement selections at
that boundary. The existing Widgets form and SELECT metadata behavior are
unchanged here. Secure Boot, raw/hybrid detection and installers remain separate.

## CP3g4b: standalone WIM classification and source ownership (2026-09-14)

**Status: implemented and verified locally as a native component.**
`IsWimImageSignature` shares Rufus's standalone Windows-image recognition with
the Windows caller: eight actual bytes matching `MSWIM` and three zero bytes,
independently of the suffix. A signature is a candidate, not a valid archive,
known Windows version or proof of bootability. Truncated prefixes and wimlib's
distinct pipable format do not match. The Windows caller now checks the actual
read count; its surrounding image policy and build systems are unchanged.

`IMAGE_FILE_SOURCE` retains an ordinary Linux descriptor and initial file facts.
Signature reads preserve its offset. `OpenWimFileDescriptor` gives native wimlib
its own descriptor for that inode through `/proc/self/fd`; the caller retains
ownership until after freeing the WIM. The final source check compares the held
descriptor and current pathname with the original device/inode/size/mtime/ctime.
Changes and cancellation invalidate dependent results. No Qt callbacks, Windows
aliases or additions to the upstream reader patch are needed. See the
[source boundary](linux-windows-image-metadata.md#cp3g4b-standalone-classification-and-native-source-ownership).

Verification:

- **38/38 CTest entries pass normally**, 30.26 seconds, including the application
  build and existing offscreen Widgets, Win32 layout and private D-Bus coverage.
- **38/38 pass with ASan, UBSan and leak detection**, 64.17 seconds, without
  sanitizer diagnostics. The native source code and upstream library are instrumented.
- **47 new functional probes** use generated metadata/content WIMs. They cover
  `.wim`, `.esd`, unrelated/no suffix and UTF-8 filenames; every short signature;
  invalid candidate archives; pipable exclusion; an outer ISO/UDF; and known,
  unknown, invalid or incomplete Windows version fields.
- Uncompressed, LZX, XPRESS, LZMS solid and integrity WIMs yield the expected EFI
  architecture. Windows-compatible case-insensitive lookup finds mixed-case
  directory/file names and reads image 1 when boot index 2 has another architecture.
  Missing EFI, unknown PE and unknown Windows version remain distinct.
- Replacement before open, after open and after extraction; unlinking; in-place
  modification with restored mtime; truncation; cancellation before/after I/O;
  symlinks; and special/missing files are exercised. Equal-size replacement files
  carry different EFI architectures. Every probe checks descriptor ownership and
  cleanup; a failed source check suppresses the observed architecture.
- **126 source component checks** cover EINTR, one-byte reads, read errors,
  premature EOF, mutation during signature reading, cancellation, offset/flags,
  symlink retargeting and repeated close. The existing shared classifier now has
  **180 checks**, including 28 signature checks. The WIM ownership suite has
  **1,857 checks**, including descriptor argument/type rejection.
- Both genisoimage and cdrtools mkisofs pass the 47 functional probes. Full
  sanitizer coverage uses genisoimage. Linux builds and test registration use
  CMake; Ninja uses default parallelism. No USB, mounts or root access are used.

Reproduce with the upstream source and builds recorded in CP3g3d:

```sh
export LD_LIBRARY_PATH=/tmp/rufus-cp3g2-wimlib/usr/lib
cmake -S linux -B /tmp/rufus-linux-qt6-build
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure
cmake -S linux -B /tmp/rufus-linux-qt6-sanitizers.ssKskZ
cmake --build /tmp/rufus-linux-qt6-sanitizers.ssKskZ
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir /tmp/rufus-linux-qt6-sanitizers.ssKskZ --output-on-failure
```

Select the focused tests with
`-R 'rufus_image_source|rufus_standalone_wim|rufus_image_file|rufus_wim_contract'`.
Normal and sanitizer build/test logs are `/tmp/rufus-cp3g4b-build.log`,
`/tmp/rufus-cp3g4b-incremental-build.log`, `/tmp/rufus-cp3g4b-tests.log`,
`/tmp/rufus-cp3g4b-sanitizers-build.log` and
`/tmp/rufus-cp3g4b-sanitizers-tests.log`.

Upstream still requires `realpath` to succeed when opening, so an already-unlinked
file returns `WIMLIB_ERR_NO_FILENAME`; tests require rejection without retrying
the original pathname. Source checks detect changes at the check, not an immutable
snapshot or subsequent writes. Existing extraction bounds/cooperative cancellation
limits still apply. Only generated content is qualified. Native Windows runtime,
remote CI, Secure Boot, media writing and actual boot tests remain deferred.
Changes are local and uncommitted.

**Deferred by the subsequent raw-workflow priority decision:** integrate standalone WIM/ESD analysis into the existing Qt
job/controller and SELECT tests. Initialize Windows-compatible WIM matching before
workers, keep image-1 metadata/EFI queries and independent unknown/missing/failed
states, reject changed/cancelled/obsolete results, and test switching back to ISO
or unknown files. The Win32-mapped Widgets form remains unchanged. Do not derive
ISO firmware bootability from an installation WIM's boot manager or clear Secure
Boot's pending state. Stop at this integration checkpoint before installer work.

## CP5: raw copy, flush and verification on disposable files (2026-09-14)

The user chose to prioritize one complete writing workflow and pause additional
image-analysis work. `linux/platform/raw_write.c` now supplies a native operation
that copies the retained source from offset zero, flushes it, and compares every
written byte with a fresh source read. It reuses the source identity/change checks
and the job's direct cancellation query. There are no I/O abstraction classes,
new callback registries or Windows changes. Linux CMake selects the implementation.

Targets must be existing regular files opened read/write, large enough and distinct
from the source. Append/direct-I/O modes and special files are rejected. The caller
retains both descriptors and offsets; no target creation, truncation or tail writes
occur. Two 1 MiB buffers bound memory. Partial reads/writes and EINTR are handled.
After any successful writes, cancellation/error still attempts a flush. The result
retains written/verified counts, failure phase and a separate flush error. Success
requires complete byte comparison and final source/target checks.

The functional test independently inspects exact output and the untouched tail.
Its 39 cases cover tiny/sector/chunk-boundary sizes, partial I/O, EINTR, early EOF,
read/write/flush failures, cancellation during copy/flush/verify, corrupt read-back,
source replacement/modification, source-target aliases, insufficient capacity,
invalid modes and recovery after a partial write. Tests only create temporary files.

Verification: **39/39 CTest entries pass normally (28.62 seconds) and with
ASan/UBSan/leak detection (60.70 seconds)**, with no sanitizer diagnostics.
The new 39-case writing test takes 0.21 seconds normally and 0.67 seconds under
sanitizers. Both application builds pass; Ninja uses default parallelism.

```sh
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build -R '^rufus_raw_write$' --output-on-failure
```

The full suite uses the existing source/build setup and optional comparator
environment from CP3g4b. Logs are `/tmp/rufus-raw-write-build.log`,
`/tmp/rufus-raw-write-tests.log`, `/tmp/rufus-raw-write-sanitizers-build.log` and
`/tmp/rufus-raw-write-sanitizers-tests.log`. Changes remain local and uncommitted.

Device access, raw-image compatibility approval, mounted-volume/source-on-target
checks, native authentication and Qt progress/state integration remain the next
boundary. START stays disabled. Buffered read-back can use the OS cache; this is
not physical durability or boot qualification. Windows and upstream wimlib are
unchanged. Existing bootloader and image-analysis coverage is retained.

**Next:** build the first supported raw write plan and acquire/revalidate an
authorized UDisks target, then invoke this operation from a named Qt worker with
progress, cancellation and accurate completion states. Keep device writing gated
until virtual USB integration passes. Follow with the fixture's VM boot test;
standalone WIM, extraction modes and broader format support remain deferred.

## CP4/CP7: raw plan and native target acquisition (2026-09-14)

`RawWritePlan` retains the source descriptor and selected native device identity.
The first mode accepts `.img` files with whole 512-byte sectors and the `55 AA`
marker used by Rufus's `AnalyzeMBR` / `is_br` check. It checks capacity and source
changes, without claiming general raw/container recognition or bootability.
Discovery and planning share one device decoder; discovery resolves the service
owner after allowing D-Bus activation, and snapshots target that unique owner.

`UDisksTarget` checks a plan without prompting. Only the later `authorize()` call,
reserved for a controller-confirmed plan, requests `OpenDevice("rw")` with
`O_EXCL | O_SYNC | O_CLOEXEC` and interactive authentication allowed. UDisks/polkit
owns the prompt; the GUI has no password or root process. Fresh snapshots before
and after authorization reject identity/source changes, mounted filesystems,
source-on-target storage, swap and known encrypted/RAID/layered targets. There is
no automatic unmounting or general storage-stack implementation in this increment.
See [OpenDevice](https://storaged.org/doc/udisks2-api/latest/gdbus-org.freedesktop.UDisks2.Block.html)
and [interaction options](https://storaged.org/doc/udisks2-api/latest/udisks-std-options.html).

The returned descriptor must be a writable block device with the expected
device number, capacity and sector alignment. Ownership is transferred once.
`WriteRawImageToDevice` repeats kernel/source validation and calls CP5's existing
copy/flush/verify loop; the regular-file entry point retains its previous behavior.
UDisks service loss, authorization denial/dismissal and late replies fail or
cancel acquisition. Discarding a pending call prevents work from starting, but
`OpenDevice` provides no RPC to dismiss an already displayed native prompt.

The private-bus fixture passes real Unix descriptors for disposable regular
files. Test-only linker wrappers simulate block type and ioctl facts for that
single inode; production has no test mode. **37 functional cases** cover a
complete acquisition/write/read-back handoff with an unmounted filesystem,
source preservation and untouched target tail; input/target rejection; malformed,
denied, dismissed or unavailable authorization; invalid descriptors; service/bus
loss; changes before/during/after acquisition; cancellation and timeout. Late
replies and successful ownership transfer are checked for descriptor cleanup.
All failure cases independently check that target contents remain unchanged.
The earlier 39 raw-I/O fault cases and 31 discovery cases still run.

Normal verification: **40/40 CTest entries pass (34.49 seconds)**, including
the application, offscreen Widgets/layout and optional wimlib comparator.
**40/40 also pass with ASan, UBSan and leak detection (68.60 seconds)**, without
sanitizer diagnostics. The new target test takes 6.11 seconds normally and
7.34 seconds under sanitizers.
Builds use CMake and Ninja's default parallelism. No host disks, system bus,
root privileges, Windows source changes or upstream wimlib changes are involved
in this checkpoint. Changes remain local and uncommitted.

```sh
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_raw_write|rufus_udisks' --output-on-failure
```

Full-suite setup and comparator environment remain as recorded in CP3g4b.
Logs: `/tmp/rufus-target-build.log`, `/tmp/rufus-target-tests.log`,
`/tmp/rufus-target-sanitizers-build.log` and
`/tmp/rufus-target-sanitizers-tests.log`.

**Next:** connect explicit confirmation and the retained plan/descriptor to a
named Qt worker, with progress, cancellation, failure and verified completion
states. Keep the Win32-mapped form. START remains disabled until that integration
and the virtual USB checks pass. Real polkit prompts, kernel exclusivity/automount
behavior and USB removal still need the guest fixture; a separate VM boot test
then establishes the first supported bootable workflow. Buffered read-back is
not proof of physical durability. CP7's desktop criterion remains open.

## CP8a: raw writing through Qt Widgets (2026-09-14)

The real Widgets path now connects SELECT, START, plan confirmation, native
descriptor acquisition, writing, flushing and complete read-back verification.
`RawWriteController` owns the plan/acquisition lifecycle; `RawWriteJob` runs the
existing C writer under `RufusJob`'s serialization and reporting adapter. Analysis
and writing cannot overlap through selection controls. No new callback registry,
Windows modifications or wimlib work is introduced.

SELECT recognizes the narrow raw `.img` path; START prepares a fresh retained
plan. The original `MSG_003` warning uses OK/Cancel with default Cancel. During
writing, CLOSE/Escape/window close use `MSG_105`, Yes/No and default No. The
form's layout stays unchanged; DD mode clears the unused formatting checks.
Native authorization remains outside the GUI. Tests explicitly provide a private
bus; the application leaves the writer unconfigured, keeping START disabled
until guest-device qualification. There is no runtime bypass flag.

The existing C count-reporting entry point supplies copy/flush/verify progress,
coalesced by the existing 50 ms Qt adapter. Copy and verification each account
for half the bar; 100%/READY requires verified bytes, final checks and worker
cleanup. Cancellation keeps the window busy while copied data is flushed and
the worker joins. Partial counts and flush errors remain available in the log.
Target/service invalidation requests interruption and ends as failure; late
authorization replies and obsolete confirmations cannot start writing. Workers
release target ownership before completion, then the controller releases the
retained source. Cancelled/failed workers can be followed by a fresh operation.

**20 functional cases** operate actual Widgets against the private UDisks fixture
and disposable storage with test-only kernel facts. They check exact written
bytes and untouched tails, confirmation rejection/retry, changed inputs/targets,
authorization denial/dismissal/unavailability, late descriptors, cancellation
during copy/flush/verify, declined cancellation, restart after cancellation,
removal, partial write failure, flush failure, corrupt read-back and teardown.
Worker I/O is paused with bounded test-only semaphores so UI responsiveness and
cleanup can be checked deterministically. GUI delivery and every progress phase
are checked. The confirmation screenshot was inspected; this does not replace
native desktop or Windows visual qualification.

Verification: **41/41 CTest entries pass normally (38.00 seconds) and with
ASan/UBSan/leak detection (74.11 seconds)**, without sanitizer diagnostics. The
new Widgets test takes 3.62 / 5.43 seconds respectively. The final cancellation
title is also covered by focused normal and sanitizer reruns. CMake builds the
application and tests; Ninja uses default parallelism. Logs are
`/tmp/rufus-qt-write-tests.log`, `/tmp/rufus-qt-write-sanitizers-tests.log`, and
the corresponding build/final-test logs. Changes remain local and uncommitted.

```sh
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build -R '^rufus_raw_write_widgets$' --output-on-failure
```

**Next checkpoint: CP6/CP7/CP8 guest qualification.** Build a reproducible Linux
VM with a file-backed emulated USB device. Exercise native UDisks/polkit prompts,
exclusive access, copy/flush/read-back, removal and post-write device discovery
using this same controller/worker. Then enable its connection in the application
and run CP9's separate boot test. Physical durability, Windows validation,
additional formats and extraction remain outside this checkpoint.

## CP8b / CP6–CP9: native USB guest and first BIOS boot (2026-09-14)

The narrow raw `.img` workflow is now enabled in the Linux application.
`main.cpp` supplies the system bus to the existing writer; its confirmation,
planning, native descriptor acquisition and worker implementation are unchanged.
No Windows source, form layout or wimlib changes were needed for this increment.

The optional CMake targets `test_usb_guest` and `check_usb_mount` run only in the
[disposable guest recipe](../linux/tests/vm/README.md). They reject host execution
with exit 77. The Qt test uses actual Widgets, production discovery/acquisition,
and the C writer, without simulated kernel facts. The small root-only fixture
helper checks the kernel's mount result against a fixed guest partition; it is
not part of the application. Normal CTest never connects to the host system bus
or runs these executables as device tests.

The offline Arch/CachyOS recipe assembled 273 cached packages, a fresh fixture
account, systemd/logind, UDisks2 and native KDE/polkit authentication. No packages
were installed on the host. The GUI runs as UID 1000 and proves that direct
`O_RDWR` access is denied before requesting an authorized descriptor. The only
polkit rule chooses the disposable administrator; it does not bypass the normal
authentication requirement. The native prompt receives the public fixture
password through the external test driver. The application never receives it.
Native authentication and completed-window screenshots were inspected.

Five QtTest entries pass (three workflow cases plus setup/cleanup, 13.992 seconds):

- Native prompt dismissal produces zero reported written bytes; retry through
  the real prompt succeeds. Copy/flush/read-back verifies all 17,825,792 source
  bytes before reporting 100%/READY and releasing the worker/descriptors.
- Linux discovers the written FAT16 partition and label after the last writable
  handle closes. No explicit rescan implementation was necessary. UDisks updates
  its medium identity, so the current conservative UI clears the selection and
  requires selecting the device again; the successful result remains visible.
- During a second, throttled write, a competing guest mount returns kernel
  `EBUSY`. The actual CANCEL confirmation interrupts copying, flushes the partial
  write and joins the worker. Mounting then succeeds and is immediately undone,
  proving that ownership was released. The cancelled run never reports 100%.
- QMP removes the emulated USB during another write. Native I/O/flush errors end
  as failure, discovery removes the target, and the worker finishes without
  claiming success. The GUI process exits with status 0 after these assertions.

The host independently compares the entire written range and the unchanged
64 MiB target's tail. It preserves that successfully verified output before the
fault cases. A separate SeaBIOS VM boots this disk read-only and reaches the
original generated fixture's `RUFUS_RAW_BOOT_OK` serial marker, with QEMU exit
status 33. A second boot check explicitly retained and hashed the firmware; the
disk hash remains unchanged. This completes CP9 for this generated BIOS fixture.

Recorded environment: QEMU 11.1.1/KVM, Linux 6.18.50-3-cachyos-lts, Qt 6.11.2,
UDisks2 2.11.2, polkit 127, KDE agent 6.7.5, Xvfb and its software-rendered native
agent. Package/input hashes and commands are in the result manifest. Source
image SHA-256: `903d76a647695bca2ff6227836a315661f39dab89562c66cf5cd72b2c2577395`.
SeaBIOS SHA-256: `b7dea28b1ab93d43f427232f5f6ab07c828fd97765d9b1daaf1b1712b39946e2`.

The VM has only generated regular-file disks: its own system disk and the USB
fixture. No host disks, filesystem shares, network or USB passthrough are
attached. Test waits are bounded and runner errors terminate QEMU. Initial
fixture issues (a launcher tied to the serial terminal, polling native window
lookup, and rendering under Xvfb) were resolved in the harness; they required no
production adapters or abstractions.

Verification after enabling the application connection: **41/41 normal CTest
entries pass (38.40 seconds)** and **41/41 pass with ASan, UBSan and leak detection
(74.43 seconds)**, without sanitizer findings. Both CMake builds pass with Ninja's
default parallelism. Host guards, Python syntax, shell syntax and diff whitespace
checks pass. The changes remain local and uncommitted.

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_BUILD_VM_TEST=ON
cmake --build /tmp/rufus-linux-qt6-build
python3 linux/tests/vm/prepare.py --build /tmp/rufus-linux-qt6-build \
    --kernel 6.18.50-3-cachyos-lts /tmp/rufus-usb-qualification-4
python3 linux/tests/vm/run.py /tmp/rufus-usb-qualification-4 /tmp/rufus-usb-run-4
```

Use new output directories when repeating the commands. Successful artifacts:
`/tmp/rufus-usb-run-4/serial.log`, `authentication.png`, `complete.png`,
`verified-usb.raw`, `boot.log`, `manifest.json`, `firmware.json` and the recorded
QEMU commands. Regression logs: `/tmp/rufus-vm-regression.log` and
`/tmp/rufus-vm-sanitizers-regression.log`; build logs use the matching VM prefix.
The saved guest/image artifacts are temporary local files, not repository assets.

**Next bounded checkpoint:** qualify a real redistributable Linux image and its
required firmware through this raw workflow, and add live reattachment/reused-path
coverage in the existing VM. Keep extraction and additional format implementation
deferred until that result is known. The current test does not establish OS
installation, UEFI/Secure Boot compatibility, full desktop automount behavior,
physical flash durability or native Windows behavior. The guest recipe depends
on matching installed Arch/CachyOS packages and their cache; general CI guest
provisioning and minimum Qt/version coverage remain future work.

## CP9b: real Linux BIOS/UEFI boot and USB reattachment (2026-09-14)

The existing raw `.img` production path now has real Linux boot evidence in both
firmware modes. This increment changes only the optional VM harness and its
documentation. The Widgets layout, production adapters, shared C implementation
and Windows build/source files required no changes.

The guest USB fixture is now 256 MiB. After removal during a write, QMP
reattaches the same backing disk, serial, capacity, `/dev/sda` and UDisks object
path. Discovery assigns a new identity. The GUI leaves selection empty, refuses
the old selection ID, and the native target checker rejects the retained old
write plan before authorization. Selecting the newly discovered target then
allows a fresh operation. Explicit QEMU block nodes retain the medium across
unplugging; this is fixture configuration, not application hotplug handling.

The optional OpenWrt input set uses the official **25.12.5 x86/64 ext4 combined**
BIOS and UEFI archives from the
[OpenWrt release directory](https://downloads.openwrt.org/releases/25.12.5/targets/x86/64/).
The download and preparation scripts verify pinned published SHA-256 hashes.
Preparation decompresses the inputs; the application still receives ordinary
uncompressed `.img` files. Downloads, sources and generated disks stay outside
Git. Signature verification and application gzip support are not claimed.

| Raw source | Bytes | SHA-256 |
| --- | ---: | --- |
| BIOS `ext4-combined.img` | 126,353,408 | `c1b1fd769b8c73562b5a12f14bde1593189035c72ace1a8d949eb155a59c2416` |
| UEFI `ext4-combined-efi.img` | 126,123,520 | `d02a23a63276d29cee88f14ad623264124a2c1fa6adce13c4095a63afeb31f31` |

Both images pass SELECT, the Win32-mapped START confirmation, native polkit
authorization, actual block writing, flush, complete read-back verification and
100%/READY. Each write uses the same unprivileged Widgets/controller/C-worker
path as the generated fixture. The guest observer copies the closed device
before the next operation, with an explicit completion acknowledgement. The
host independently compares every source byte and the unchanged target tail.
The UEFI source is 229,888 bytes shorter; its tail must match the preceding BIOS
snapshot, including those retained bytes.

Each saved output boots as USB in a separate SeaBIOS or OVMF VM. The serial shell
reads the release file, architecture and presence of `/sys/firmware/efi`, producing
these exact output lines before orderly power-off:

```text
RUFUS_OPENWRT_BOOT_OK version=25.12.5 arch=x86_64 firmware=bios
RUFUS_OPENWRT_BOOT_OK version=25.12.5 arch=x86_64 firmware=uefi
```

The input images run Linux 6.12.94. First-boot changes go into QEMU's temporary
snapshot; the saved output disk hash must remain unchanged. The UEFI raw image
produces GPT end-of-disk warnings on the larger USB target and still boots from
its primary table. The writer preserves the official source bytes; it does not
repair or relocate partition metadata. Secure Boot is not enabled.

The serial driver requests fresh prompts until startup has replaced the initial
`(none)` hostname with `OpenWrt`, then paces command bytes into the emulated UART.
Early probes exposed console startup and input-loss issues; they were fixed in
the harness. A printed shell banner alone does not pass the test:
the command output, correct firmware mode, clean shutdown and unchanged backing
hash are all required. The generated BIOS fixture remains a separate boot check.

One repeat stalled inside OVMF before the boot loader/kernel appeared and hit
the 120-second limit. Its USB and firmware hashes match a successful UEFI run;
the cause is unresolved. The runner retains this as a failure and terminates
QEMU, with no automatic retry. Its log is preserved in
`/tmp/rufus-openwrt-run-3/openwrt-uefi-boot.log`. Successful boots establish the
tested image path, not firmware reliability across machines or repeated runs.

Verification includes **eight native QtTest entries** (six workflow cases/data
rows plus setup/cleanup, 138.242 seconds), all three output boot checks, and **41/41 normal CTest
entries (45.62 seconds)**. The native run repeats authentication dismissal/retry,
kernel mount exclusivity, cancellation and removal alongside the new checks.
The final complete runner exits 0 with both OpenWrt userspace markers and clean
power-off, as well as the generated BIOS fixture's boot marker.
The normal CMake build, host-execution guards, Python and shell syntax, and
changed-file whitespace checks pass. Production code is unchanged from CP8b's
recorded sanitizer qualification; sanitizers were not rerun for this harness
increment. Changes remain local and uncommitted.

```sh
cmake --build /tmp/rufus-linux-qt6-build
python3 linux/tests/vm/openwrt.py /tmp/rufus-openwrt-25.12.5
python3 linux/tests/vm/prepare.py --build /tmp/rufus-linux-qt6-build \
    --kernel 6.18.50-3-cachyos-lts --openwrt /tmp/rufus-openwrt-25.12.5 \
    /tmp/rufus-openwrt-guest
python3 linux/tests/vm/run.py /tmp/rufus-openwrt-guest /tmp/rufus-openwrt-run-4
```

Use new preparation/result directories for a repeat. Evidence is retained in
`/tmp/rufus-openwrt-run-4`: `serial.log`, native prompt/completion screenshots,
`verified-usb.raw`, `openwrt-bios.raw`, `openwrt-uefi.raw`, all three boot logs,
the package/input manifest, firmware copies/hashes and QEMU command records.
The final runner log is `/tmp/rufus-openwrt-qualification-final.log`; normal regression
and build logs are `/tmp/rufus-openwrt-regression.log` and
`/tmp/rufus-openwrt-build-all.log`. The recipe and dependencies are documented in
the [VM README](../linux/tests/vm/README.md).

**Next bounded checkpoint:** reuse Rufus's existing bootable-image/ISOHybrid
detection and shared policy for one Linux ISO in DD mode. Qualify its selection,
plan and boot behavior before broadening support. Preserve Win32 control/dialog
behavior and keep unavailable extraction modes unavailable. Other distributions,
OS installation, full desktop automount behavior, physical USB durability,
Secure Boot and native Windows validation remain separate work.

## CP10a: shared boot-marker detection and ISOHybrid policy (2026-09-14)

SELECT now detects DD candidates using the same marker rule as Rufus's
`IsBootableImage` → `AnalyzeMBR` → ms-sys `is_br` path. The byte predicate lives
in the existing `image_file.c` module as `IsBootImageSignature`. Windows `is_br`
uses its existing sector-reading transport and calls this predicate; Linux
classifies the first sector through the existing retained-file source. The
Windows caller change is four added/two removed lines, without a platform
conditional or new Windows build entry. The modified C translation unit compiles
locally; native Windows runtime validation remains deferred.

`OpenImageFileSource` reads at most 512 bytes without moving the file offset,
preserving short-read/EINTR/error/cancellation handling and descriptor ownership.
WIM signature and boot-marker facts remain independent. The `.img` planner
consumes this shared result, removing its duplicate marker read/comparison while
retaining its format, size, WIM exclusion and target checks. A marker alone is
not a partition-table check, executable boot-code check or boot qualification.

The Qt job retains and rechecks the input's device/inode/size/mtime/ctime before
publishing its report. Changing a marker and restoring the original timestamp
is rejected. Successful ISO metadata receives `is_bootable_img`, then runs the
existing `ApplyIsoPolicy` unchanged, using its original regex implementation.
The status/log distinguishes a DD candidate, an existing policy that requires
DD, and native ISO-mode installer checks that have not been performed. ISO
metadata remains visible; the report is not changed into a fake raw-image
selection. `.iso` writing and the mode-selection dialog remain gated at this
checkpoint. The qualified `.img` workflow remains enabled.

The user's mode-selection decision is explicit: preserve Rufus's ISOHybrid
dialog with **ISO Image mode recommended and selected by default**, and **DD
Image mode selectable**. Preserve DD-required cases and existing conditional
choices. There is no Linux-specific, Arch-specific or CachyOS-specific default.
When execution is ready, use the original `MSG_274`/`MSG_275` text and
`MSG_276`/`MSG_277` choices, with the same cancellation and persistence behavior.
See the [UI contract](linux-win32-ui.md#isohybrid-mode-selection-decision-2026-09-14).

Rufus's general ISO preference concerns editable files and normal Windows access
to the resulting USB; it is not a per-image compatibility guarantee. The
[Rufus FAQ](https://github.com/pbatard/rufus/wiki/FAQ#why-doesnt-rufus-recommend-dd-mode-over-iso-mode-for-isohybrid-images-surely-dd-is-better)
explains that preference. At this check, the
[CachyOS preparation guide](https://wiki.cachyos.org/installation/installation_prepare/)
documents raw copying under Linux, while the English
[ArchWiki Rufus section](https://wiki.archlinux.org/title/USB_flash_installation_medium#Rufus)
suggests DD if ISO mode fails. Some translated Arch pages still prescribe DD
outright. These documentation differences do not change the chosen UI behavior.

Tests cover the shared marker's size bounds and unaligned buffers, complete and
partial source reads, faults, cancellation, independent WIM classification,
cleanup and offset preservation. Six new generated-image cases exercise actual
Widgets: Arch/CachyOS/unrecognized labels with a marker, the existing SUSE DD-only
exception, the same label without a marker, and a marker-bearing non-ISO. Another
case changes the marker during analysis and restores mtime. These synthetic
labels test classification/policy separation; they are not boot qualification
of actual Arch or CachyOS releases.

Normal verification passed the other **40 CTest entries** on the full run and
the corrected Widgets suite on its focused rerun (**45 QtTest entries**, 6.02
seconds for CTest). The initial SUSE fixture failed because libarchive normalized
its PVD label; the fixture now preserves the mixed-case label used by Rufus's
existing exception. No policy change was made to accommodate it. The final full
**ASan/UBSan/leak-check run passes 41/41 CTest entries (106.89 seconds)** with no
sanitizer findings. Both CMake builds pass with Ninja's default parallelism;
existing dependency/fixture warnings remain. No native VM rerun or additional
boot qualification is claimed for this read-only classification checkpoint.

Logs: `/tmp/rufus-hybrid-tests.log`, `/tmp/rufus-hybrid-widgets-final.log`,
`/tmp/rufus-hybrid-sanitizers-tests.log`, and corresponding `*-build*.log` files.
Changes remain local and uncommitted.

**Next:** connect and test the ISOHybrid mode-selection/plan boundary using
Rufus's original choices and defaults. Keep unsupported execution paths gated;
do not advertise an unimplemented ISO mode or silently substitute DD. Add the
required native extraction pieces in bounded steps, and qualify a representative
real ISO's selected writing mode through the existing virtual USB/boot harness.

## CP10b: shared ISOHybrid choices and Qt Widgets dialog (2026-09-14)

The ISOHybrid/DD decision in `BootCheckThread` now uses
`GetImageWriteChoices` from the existing `image_policy.c` module. It returns
at most three modes, in the original order, with the first as the default:
ISO/DD, or ISO/ISO → ESP/DD when the projected size is strictly below 1 GiB,
regular EFI files are present, and the selected target is GPT with FAT.
Persistence suppresses the hybrid choice. Pure DD and DD-required images return
DD alone. The existing post-scan DD requirement is shared by these choices and
`ApplyIsoPolicy`, including reports whose `is_iso` transition the Linux UI
intentionally leaves untouched.

`GetImageWriteMode` resolves the original `SelectionDialog` row bit into a named
mode. This matters because DD is row two without ESP and row three with it.
Cancel, a missing choice and invalid/multiple row bits return no mode. The
Windows caller retains its existing localized dialog, flags and later boot
checks; it no longer duplicates the two dialog-building branches. The ESP size
constant moves to the already shared report header with the same value. No
Windows build entry or platform conditional was added. Native Windows build and
runtime validation remain deferred; Linux test results do not validate the
Win32 caller or dialog.

`ImageModeDialog` implements the ISOHybrid dialog using Qt6 Widgets: original
message and choice captions, native question icon, exclusive radio buttons,
ISO selected first and OK as the default button, followed by Cancel. Enter
accepts, arrow keys change the radio choice, and Cancel/Escape/window close
return no operation. A fresh dialog always starts at ISO. There are no authored
lambdas, custom templates, generic dialog framework or new runtime dependency.

The selection controller exposes choices only after analysis has completed.
Generated ISO tests exercise the actual SELECT report through this policy and
the Qt dialog while retaining ISO metadata and keeping the writer idle. **The
application does not open this dialog from START yet, and `.iso` writing remains
gated.** Accepting a mode alone does not prepare a target, request authorization
or start writing. Native ISO extraction and ISO → ESP are not implemented by
this checkpoint. The separate Windows ISO-to-ESP-only prompt is also outside
this checkpoint. The qualified raw `.img` workflow remains enabled.

The normal full suite passes **43/43 CTest entries (47.91 seconds)**. This
includes **384 shared C policy cases**, **13 Qt dialog test entries** and the
separate enlarged-font/2× layout run, plus **45 image-analysis Widgets entries**.
The dialog tests read English definitions directly from `res/loc/rufus.loc` to
check the title, full message and captions. They cover every row in both dialog
forms, exclusivity, default Enter, keyboard navigation and cancellation after
selecting DD. The C tests cover the 1 GiB boundary, EFI flags, GPT/FAT conditions,
persistence, DD-only policy, unknown Syslinux versions and rejected row bits.

The first focused dialog run exposed a test click in the stretched blank area
of short radio labels; Qt only activates the indicator/text area. The fixture
now clicks that area and asserts DD is selected before testing cancellation.
The default and enlarged keyboard-navigation screenshots were visually checked:
`/tmp/rufus-mode-dialog-default.png` and `/tmp/rufus-mode-dialog-scaled.png`.

The full **ASan/UBSan/leak-check suite also passes 43/43 CTest entries (76.01
seconds)** with no sanitizer findings. Its logs are
`/tmp/rufus-mode-sanitizers-build.log` and `/tmp/rufus-mode-sanitizers-tests.log`.

Normal build/test logs are `/tmp/rufus-mode-build.log`,
`/tmp/rufus-mode-build-final.log`, `/tmp/rufus-mode-focused.log` and
`/tmp/rufus-mode-tests.log`. Both builds use CMake/Ninja with default parallelism;
existing dependency/fixture warnings remain. No new virtual USB or boot
qualification is claimed. Changes remain local and uncommitted.

**Next bounded checkpoint:** implement native ISO file extraction into
disposable test storage, reusing Rufus's shared traversal/readers and file rules.
Check extracted names/content, cancellation, source changes and I/O failures
before adding target formatting and bootloader installation. Connect the
original mode dialog to START when the corresponding execution path is ready;
do not change the ISO default or silently substitute DD to bridge that gap.

## CP10c paused: native ISO tree extraction (2026-09-14)

Historical pause record; resumed and completed in the 2026-09-15 entry below.
Temporary files mentioned here were cleared by a reboot; fresh build/test paths
are recorded with the completed checkpoint.

Paused at the user's request. **Work in progress, not a validated checkpoint.**
The passing CP10b test results above predate these changes.

Current local edits separate `WalkIsoFiles`/`WalkUdfFiles` from scan accounting
inside the existing `src/iso_scan.c` and header. A synchronous internal C visitor
lets scanning and native extraction share traversal without involving Qt.
`linux/platform/iso_extract.c` and `.h` add the initial ordinary-file/directory
extractor, using the retained source, shared ISO/UDF readers, an empty destination
directory, a preflight pass and descriptor-relative output creation. Symlinks,
special entries and incomplete/deep trees are rejected in this initial scope.
The root `ldlinux.sys` exclusion moves into shared file properties and is used
by the existing Windows extraction check. Linux CMake adds `rufus_iso_extract`;
it is not connected to the GUI or START.

The CMake build completed successfully; its log is `/tmp/rufus-extract-build.log`. No extraction
tests have been added or run, and the existing regression/sanitizer suites have
not been rerun against this refactor. Do not treat this code as ready to use.
A copy of the pre-refactor scanner is `/tmp/rufus-cp10c-iso_scan-before.c` for
review only; the existing worktree contains earlier uncommitted work too.

On resume: review the traversal refactor and initial extractor, confirm the build,
then add generated-image functional tests for ISO/Rock Ridge/Joliet/UDF, nested
and empty directories, exact contents and the reserved root loader. Cover
unsupported entries, cancellation, source changes, short/failed writes, flush
and close errors, cleanup and descriptor ownership. Run the existing regression
suite and sanitizers before calling CP10c complete. ISO START remains gated;
formatting, bootloader installation and configuration patching remain later work.

## CP10c: native ISO/UDF tree extraction (2026-09-15)

Completed the paused ordinary-file extraction component. `WalkIsoFiles` and
`WalkUdfFiles` now supply shared traversal inside the existing `iso_scan.c`
module. Scan accounting is a named synchronous C visitor; native extraction is
another. This keeps the original accounting, selected image view, directory
limits and UDF iterator ownership in one place. The visitor has no Qt or GUI
dependency; reporting and cancellation retain their named host entry points.
No Windows build entries or platform conditionals were added for this split.

Linux's `ExtractIsoTree` retains the source descriptor and opens the selected
ISO/UDF view through that descriptor. It rechecks source identity before,
during and after copying, including pathname replacement and content changes
with restored mtime. A preflight walk rejects unsupported links/special entries
and incomplete/deep trees before creating output. The second walk copies into
a caller-owned private empty directory using descriptor-relative operations,
exclusive file creation and no symlink traversal. Files and directories are
flushed. A failed current file is removed; earlier completed entries remain and
the whole output is incomplete. Data buffers stay fixed at 1 MiB plus the
existing reader's 32 KiB cache.

The root `ldlinux.sys` exclusion is now a shared file property, used by both the
Windows extraction check and Linux. Nested loaders remain ordinary files.
There are no payload substitutions or configuration rewrites in this component.

**57 generated-image functional cases pass**, with independent inspection of
names, directory structure and complete contents. Coverage includes:

- ISO9660, Rock Ridge, Joliet and UDF; Unicode, spaces, nested/empty directories,
  empty files, chunk boundaries, and fragmented UDF short/long allocations.
- A UDF name differing from the ISO view, root-loader exclusion, preservation
  of nested loaders and unchanged configuration contents.
- Cancellation before analysis, after analysis and during copying/flushing;
  successful retry in the same process after a cancelled write.
- Source mutation/replacement, short writes, EINTR, zero/failed writes, failed
  reads, file/directory flush failures and close failures. These faults are
  exercised for both ISO and UDF, with exact partial counts and descriptor checks.
- Existing output preservation, a replaced parent directory, unsupported
  entries, invalid/truncated images and deep-directory rejection.

Fault injection exists only in the test executable through link-time wrappers.
The extractor has no testing switches and does not access devices or mounts.
The shared file-property suite now passes **703 checks**. Existing tree, analysis,
Widgets, mode-dialog and raw-writing regressions all pass after the refactor.

The full normal suite passes **44/44 CTest entries (53.13 seconds)**. The full
**ASan/UBSan/leak-check suite passes 44/44 (78.21 seconds)** with no sanitizer
findings. Both include the optional independent system-wimlib comparison.
Normal and sanitizer builds use CMake/Ninja with default parallelism. Existing
dependency warnings remain. Windows build/runtime validation is still deferred;
these Linux results do not qualify the Windows application. No new VM boot or
physical USB qualification was performed.

After the reboot, test-only dependencies were restored under
`/tmp/rufus-test-deps`, without installing system packages. The CMake provider is
the upstream wimlib 1.14.5 source release (SHA256
`84221a3abd5b91228f15f8e6065c335a336237b5738197b75bf419eea561a194`).
`RUFUS_MKISOFS` selects `/tmp/rufus-test-deps/usr/bin/mkisofs`;
`CMAKE_PREFIX_PATH` selects `/tmp/rufus-test-deps/usr` for the optional comparator,
with `LD_LIBRARY_PATH=/tmp/rufus-test-deps/usr/lib` during its tests.
Normal build/test logs are `/tmp/rufus-extract-build-final.log` and
`/tmp/rufus-cp10c-ctest.log`. Sanitizer logs are
`/tmp/rufus-cp10c-sanitizer-build.log` and
`/tmp/rufus-cp10c-sanitizer-tests.log`. These temporary paths are not durable
artifacts. Changes remain local and uncommitted.

**Scope and next checkpoint:** this qualifies ordinary-file extraction into
disposable directories. Filesystem-specific naming, timestamps, symlink policy,
WIM splitting, payload replacement, configuration patching and bootloader
installation remain. Next prepare a native FAT32 filesystem in a disposable disk
image and inspect its layout/filesystem independently. Integrate extraction and
boot support in subsequent steps. Application `.iso` START remains gated, and
the original ISO default/selectable DD behavior is unchanged.

## CP10d: native FAT32 formatting in image files (2026-09-15)

`linux/platform/fat_format.c` adds one concrete native adapter,
`FormatFat32Image`. It quick-formats an explicit region of an existing regular
file with dosfstools `mkfs.fat`, passing the retained descriptor through
`posix_spawn` instead of reopening the caller's pathname. Structured arguments
specify offset, size, cluster size, label and volume ID, with two FATs and backup
boot metadata. No shell or new generic process framework is involved.
The [dosfstools 4.2 implementation](https://github.com/dosfstools/dosfstools/blob/v4.2/src/mkfs.fat.c)
was checked for offset/block units, argument validation, alignment and writes.

The adapter validates the file range and access flags before starting. Its
current subset uses 512-byte sectors, explicit power-of-two clusters from 512
through 32768 bytes, and already normalized uppercase ASCII labels. It checks
that the region can hold a standard FAT32 cluster count; choosing Rufus's
allowed/default cluster sizes and full label normalization remain caller work.
No alternative UI defaults or filename-based media choices were introduced.

Cancellation uses the existing named C job query. Diagnostics are capped at
4095 bytes while excess output is drained. Cancellation or a 120-second timeout
sends TERM, escalates to KILL after 500 ms when needed, and reaps the child.
After any started operation the target is flushed, including failure/cancel
paths; the result retains child status, signal and any flush error. Success
also requires unchanged target size, expected boot-sector fields and a readable
root through Rufus's existing `libfat`. Its FAT structures/readers are reused
unchanged. The caller retains descriptor flags and seek position.

**35 functional cases pass.** Real native formats cover 64 MiB/512-byte clusters,
128 MiB/1024-byte clusters, 512 MiB/4096-byte clusters and 2.25 GiB/32768-byte
clusters, including a volume at offset zero and partition-like regions at 1 MiB.
Tests independently decode geometry/label/ID, compare both FATs and backup boot
metadata, run `fsck.fat -n` on an exact sparse copy of the volume and use mtools
to list the original image at its offset. All bytes preceding and following
the requested region are checked against guards. Native track rounding is
reported explicitly; less than 63 sectors can remain unused inside the region.

Other cases cover a replaced pathname while its original descriptor remains
open, unsupported geometry/labels/access modes, non-regular descriptors, missing
tools, nonzero exits, signals, excessive/early-closed output, changed file size,
flush failure/EINTR, cancellation, forced termination, timeout and recovery.
They check descriptor ownership and that no child remains. Controlled fake
formatters and fault wrappers exist only in tests; timeout testing advances
the test clock rather than waiting two minutes. Tests use only disposable files,
with no device, mount, root prompt or production testing switch.

The full normal suite passes **45/45 CTest entries (56.02 seconds)**. The full
**ASan/UBSan/leak-check suite passes 45/45 (118.08 seconds)** with no sanitizer
findings, including all 57 ISO/UDF extraction cases and the optional independent
system-wimlib comparator. Both CMake/Ninja builds use default parallelism. The
new formatter/probe compile without warnings; existing upstream wimlib warnings
remain. Logs are `/tmp/rufus-cp10d-build-final.log`,
`/tmp/rufus-cp10d-tests.log`, `/tmp/rufus-cp10d-sanitizer-build.log` and
`/tmp/rufus-cp10d-sanitizer-tests.log`. These temporary logs are not durable.
Build/dependency paths are the same as CP10c. New test executables `fsck.fat`
and `mdir` come from the already required dosfstools and mtools packages.

**Next bounded checkpoint:** create one Rufus-compatible partition layout in a
disposable image using an established Linux facility, independently inspect the
table, and format only its recorded partition extent. Real-device formatting
through authorized services, mounting and extraction integration follow later.
This checkpoint does not create a partition table, erase old data or install a
bootloader. ISO START remains gated. No Windows code/build files or Qt UI were
changed, and no new VM/USB/boot qualification is claimed. Changes are local and
uncommitted.

## CP10e: shared placement and native GPT images (2026-09-15)

`src/partition_layout.c` extracts Rufus's first-partition offset and main-partition
length calculations from `drive.c`. Windows now calls those same functions;
its disk APIs, formatter and remaining partition choices stay in the Windows
implementation. The shared code preserves the modern 1 MiB start, legacy BIOS
track/cluster adjustment and main-length rounding order. Windows build manifests
register the new source/header. This is a small policy extraction, with no Qt
dependency or new platform conditionals.

Linux's `CreateGptImagePartition` uses native libfdisk through a retained regular
file descriptor. It creates one ordinary GPT "Main Data Partition" with the
Microsoft Basic Data type, 128 entries and Rufus's shared placement calculations.
The initial subset accepts 512-byte sectors and explicit geometry/cluster sizes.
It checks the prepared table, rechecks file size before writing, flushes every
attempted write and verifies the result through a fresh library context. Recovery
from a damaged GPT copy is rejected during that verification. The caller retains
the descriptor, flags and seek position; cancellation uses the existing C entry
point. No device, mount, privilege prompt or bootloader operation is involved.

The [libfdisk descriptor API](https://www.kernel.org/pub/linux/utils/util-linux/v2.41/libfdisk-docs/libfdisk-Context.html)
supports this image-only checkpoint without a loop device or helper process.
[UDisks partition creation](https://storaged.org/doc/udisks2-api/latest/gdbus-org.freedesktop.UDisks2.PartitionTable.html)
can adjust requested offsets and sizes, so its returned bounds must be checked
when integrating real devices. UDisks2/polkit remains the first choice for their
authorized operations. Linux CMake requires libfdisk development files through
pkg-config (`fdisk >= 2.35`); CI also installs the `sfdisk` test utility.

Integration caught two behavior differences. Rufus can produce an odd number
of 512-byte sectors, so the FAT32 adapter now accepts sector-aligned extents and
disables mkfs.fat's additional alignment for odd lengths. The explicit block
count remains in KiB; the filesystem may leave its final sector unused, without
changing the partition bounds. Replacing an existing GPT also reset libfdisk's
first-usable-sector override. In-memory header settings now preserve sector 34
on both initial creation and replacement.

**2,527 portable checks** compare the shared arithmetic with the original
formulas across sector, track, cluster and disk sizes, including legacy starts,
reserved tails and invalid/overflow boundaries. **27 native GPT functional
cases pass**, including **three GPT-to-FAT32 integration runs**. Tests decode
both GPT copies independently, check header/table CRCs, type/name/bounds and
protective MBR, and run `sfdisk --json` and `--verify`. They inspect all bytes
outside table metadata or the formatted partition, use `fsck.fat` and mtools on
the actual recorded extent, and recreate GPT without altering the FAT boot
sector. The separate formatter suite now passes **37 cases**.

Failure coverage includes invalid targets/options, pathname replacement,
cancellation before/after writing, target resizing, allocation/write/flush
failures, EINTR and damaged primary or backup GPT metadata. Test-only link
wrappers inject failures; production code has no fault switches. Descriptor
ownership and unchanged images before any attempted write are checked.

The full normal suite passes **47/47 CTest entries (45.04 seconds)**. The full
**ASan/UBSan/leak-check suite passes 47/47 (84.63 seconds)** with no findings.
Both include the optional system-wimlib comparison. The system libfdisk binary
itself was not rebuilt with sanitizers. CMake/Ninja builds use default
parallelism; authored checkpoint code builds without warnings, while existing
upstream wimlib warnings remain. Native tools tested here are util-linux 2.42.3
and dosfstools 4.2; the CMake wimlib provider remains upstream 1.14.5.

Logs are `/tmp/rufus-cp10e-build-final.log`, `/tmp/rufus-cp10e-tests.log`,
`/tmp/rufus-cp10e-sanitizer-build.log` and
`/tmp/rufus-cp10e-sanitizer-tests.log`; these temporary paths are not durable
artifacts. Windows project XML and source/object registrations were checked,
but Windows build/runtime validation remains deferred. No new VM boot or
physical USB qualification was performed. Changes remain local and uncommitted.

**Next bounded checkpoint:** qualify the existing tree extractor on a mounted
FAT32 volume inside the disposable USB guest, checking filenames, contents and
cleanup against actual filesystem behavior. Real-device orchestration, other
partition layouts, filesystem-specific policy and boot support remain later
work. Application `.iso` START stays gated; this checkpoint does not establish
bootability or erase old filesystem signatures/data.
