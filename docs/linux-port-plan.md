# Rufus Linux port implementation plan

Updated: 2026-09-15

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

**Current priority, revised 2026-09-15:** the narrow raw-image workflow has
passed virtual USB and BIOS/UEFI boot qualification. Build the native ISO-mode
path in small tested steps, keeping Rufus's ISO default and selectable DD mode.
CP10c supplies ordinary-file extraction into disposable directories; CP10d
formats a FAT32 region in disposable image files using native tools. CP10e adds
a GPT main partition using common Rufus placement arithmetic and native libfdisk,
with tests formatting its actual extent. Device-service integration and boot
support remain. Further image-analysis variants,
including standalone WIM/ESD SELECT integration, stay deferred. Completion is
measured by supported workflows.

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
| Adapters | Use small Windows-to-Linux API adapters and Linux implementations of existing Rufus interfaces where the semantics map cleanly. They adapt platform operations; they do not emulate GUI dependencies. Wine/Winelib are excluded. |
| Build | Linux is strictly CMake-based, with its entry point under `linux/` and Ninja's default parallelism. Windows retains Rufus's existing Visual Studio/MSBuild and MinGW/Autotools build paths. |
| Execution | Keep the GUI unprivileged and responsive. Serialize jobs that use Rufus's process-global state. |
| Privileged work | Request authorization through the desktop's system authentication agent, using UDisks2/polkit on Linux. |
| Coding style | Follow the direct style of `allparts_new/src`: named functions, concrete data, explicit control flow, no lambdas, and templates only where their benefit is substantial. |
| Performance | Keep disk and image loops efficient, bound notification work, and measure representative workloads. |
| Delivery | Implement and validate useful media paths incrementally. Do not claim full Rufus feature parity from the initial releases. |
| Checkpoints and tests | Work on one bounded checkpoint at a time, with explicit acceptance tests. Use simulated devices, temporary files and QEMU USB emulation while physical USB media is unavailable. |
| ISOHybrid mode choice | Keep Rufus's existing dialog, ISO-mode recommendation/default and selectable DD mode. Preserve its DD-required exceptions and conditional choices. Do not introduce a Linux-specific or filename-based distro default. |

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
| ISOHybrid mode dialog | CP10b shares the original ordered mode choices between Windows and Qt, with ISO as the default, conditional ISO → ESP, DD and cancellation. The Qt dialog component is tested but not opened by application START until ISO execution is ready. |
| Native ISO/UDF extraction | CP10c shares traversal between scanning and extraction, reuses the existing file readers, and copies ordinary trees into private empty directories. Generated-image tests cover exact output, cancellation, source changes and I/O failures. Filesystem integration, boot support and ISO START remain. |
| Native FAT32 preparation | CP10d formats an explicit region of a retained regular file with mkfs.fat, then flushes and checks it using Rufus's FAT reader. CP10e handles sector-aligned boundaries, including odd sector counts. Real-device formatting remains separate. |
| GPT image preparation | CP10e shares Rufus's first/main partition placement calculations with Windows. Linux uses libfdisk to write and verify one ordinary GPT main partition. Raw-byte/CRC checks and FAT32 integration tests preserve both GPT copies and all bytes outside the formatted extent. Device services, additional layouts and boot support remain. |
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
| Bootable-media validation | CP9 passes for the generated BIOS/FAT16 fixture. CP9b writes official OpenWrt 25.12.5 BIOS and UEFI raw images and boots each verified output to Linux userspace in separate SeaBIOS/OVMF VMs. Other distributions, OS installation and Secure Boot remain unqualified. |

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
libfdisk on Linux and the existing Windows disk APIs. Next qualify the existing
tree extractor on a mounted FAT32 volume inside the disposable USB guest,
checking real filesystem names/content and cleanup. Device-service and boot
qualification still precede enabling ISO writing.
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

Detection/presence and selection policy now have component and generated-image
coverage. Next, install on disposable FAT images and boot representative fixtures
in QEMU, including unknown-version fallback cases. Those fixtures establish
coverage rather than universal compatibility. The selected numeric fallback
retains Rufus's empty suffix and does not establish native installer or payload
capability. That remains installation work, before destructive operations.

## Linux operations and dependency choices

Use one selected implementation per operation initially. The alternatives
below are decisions to validate against required features, not a request to
build multiple interchangeable backends in advance.

| Operation | Preferred starting point | Decision or gap to resolve |
| --- | --- | --- |
| Device discovery and hotplug | UDisks2 over QtDBus. | Device identity, transport, mount state, eligibility and service availability. |
| Authorization and opening a target | Existing UDisks2 authorization and descriptor APIs. | Verify the access mode and lifetime needed by each write path. |
| Mounting and unmounting | UDisks2 filesystem services. | Coordinate active mounts, later automounts and cleanup of resources owned by the job. |
| Partition layout | Assess UDisks2 partition APIs first. | Use libfdisk or controlled `sfdisk` execution if required layouts are not expressible. |
| Filesystem creation | Existing formatting services and `mkfs.fat`, `mkntfs`, `mkfs.exfat` as needed. | Validate support for the exact filesystem, label and cluster-size options exposed by the UI. |
| WIM inspection, splitting and updating | Linux wimlib. | Preserve the Rufus choices and parameters; verify bundled-versus-system differences. |
| ISO/UDF access | Rufus's libcdio-based approach. | Choose bundled or system libraries after the behavioral audit. |
| Raw image writing | A small operation using an authorized descriptor and established POSIX I/O. | Partial I/O, size limits, cancellation, flush, verification and accurate error reporting. |
| Boot record/loader installation | Reused Rufus and bundled boot components where applicable. | Filesystem-specific placement, versions, offsets and actual boot validation. |
| Uncovered privileged operations | A narrowly scoped helper only when a concrete service gap is established. | Keep its interface operation-specific; do not run the GUI as root. |

For external tools, use structured arguments without a shell, check exit status
and diagnostics, and implement bounded cancellation/cleanup. Check required
capabilities before changing a target. Source analysis should use library file
access without requiring privileged mounts where the chosen library supports it.

CP10e uses libfdisk (2.35+) for regular image files: its descriptor API and
explicit partition bounds avoid loop devices or privileges in this checkpoint.
UDisks2 remains the first option for device operations; its CreatePartition API
may adjust requested boundaries, so device integration must check the returned
extent before formatting. This does not authorize libfdisk to open host disks.

Every destructive path must revalidate the target identity and capacity, ensure
the source does not reside on the target being erased, and handle disconnects
and I/O failures. These are responsibilities of the job/controller even when
the underlying operation is delegated to a system service.

## Privileges and desktop authentication

Some partitioning, formatting, mounting and device-access operations require
privileges. Keep the Widgets application running as the ordinary user and
delegate those operations to an authorized service. Start with UDisks2, which
performs authorization checks through polkit. The session's authentication
agent supplies the password dialog and chooses the identity according to
system policy. This may be the current user, an administrator or root; some
authorized actions need no prompt. [UDisks2 authorization](https://storaged.org/doc/udisks2-api/latest/udisks-polkit-actions.html),
[polkit authentication agents](https://polkit.pages.freedesktop.org/polkit/polkit.8.html).

The application requests permission for a concrete operation. It must not
collect, store or log a root password, implement its own password dialog, or
feed passwords to `sudo`/`su`. An authorization prompt is separate from the
application's confirmation of which device will be erased.

### User flow

1. Analyze the source and prepare the write plan as the ordinary user.
2. Show the target identity and planned changes; the user chooses to proceed.
3. Enter Authorizing and request the required operation asynchronously, allowing
   the system authentication agent to interact. Keep the window responsive.
   UDisks2's `auth.no_user_interaction` option must not suppress interaction for
   this user-initiated request; background discovery should not trigger prompts.
   [UDisks2 options](https://storaged.org/doc/udisks2-api/latest/udisks-std-options.html).
4. On success, revalidate the target after the possible authentication delay
   and continue only the authorized work. Later protected operations still use
   their own required service checks; do not treat one successful prompt as
   unrestricted root access.
5. Treat cancellation and denial as expected outcomes. Preserve selections,
   stop before the unauthorized step, and explain what happened without an
   automatic retry loop. If earlier steps changed the device, report that
   partial state accurately and perform the cleanup still permitted.

Avoid unnecessary duplicate requests, but respect system policy if multiple
actions require separate authentication. Do not install blanket authorization
bypasses to achieve a single prompt. Set timeouts with human interaction in
mind and distinguish an unanswered prompt from an I/O failure.

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

If a needed operation cannot be expressed through UDisks2, use a small,
installed helper with narrowly defined operations and system authentication.
Evaluate polkit-authorized D-Bus activation or `pkexec` for that specific gap.
The privileged side must validate the request itself, including target identity
and allowed operations. Elevate the helper only; do not provide arbitrary
command execution or pass GUI secrets through arguments or environment values.
[pkexec](https://polkit.pages.freedesktop.org/polkit/pkexec.1.html).

Authentication behavior is part of the Phase 3 acceptance criteria and the
packaging/dependency matrix. Tests must cover approval, denial, cancellation,
missing agent/service, authorization expiry or a second challenge, and target
removal while the prompt is open. Real prompts are manual integration tests;
automated tests exercise service responses without obtaining passwords.

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
