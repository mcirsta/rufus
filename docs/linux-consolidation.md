# Production consolidation pass

Authorized and completed 2026-09-27. Feature expansion was paused for this pass. Scope is
code contributing to the shipped GUI, helper and required runtime tools; tests,
probes, documentation and unlinked adapters do not count toward simplification.
Baseline: 14,644 unique Linux implementation lines in the GUI/helper build input
graphs (headers excluded). Counts are supporting evidence, not acceptance criteria.

## Work and acceptance

- [x] Consolidate repeated writable-target descriptor/kernel checks. Preserve
  each caller's extent/sector/filesystem rules and fresh checks around operations.
- [x] Use the existing single authorized helper for all GUI DD writes. Remove
  direct UDisks OpenDevice/local-worker lifecycle and qualify unmounted/mounted,
  denial, cancellation, source/device changes, verification and exit-before-READY.
- [x] Share concrete layout/formatter planning between GUI preflight and helper.
  The helper still recomputes and validates untrusted requests independently.
- [x] Review helper format workflows, controllers, extraction and tool validation
  for remaining duplicated responsibilities. Consolidate useful common operations;
  keep different native operations explicit and avoid a new framework.
- [x] Run affected normal/sanitizer component and GUI/helper suites, then a
  representative simulated-device functional check where available. No physical
  USB writes or additional Windows-customization qualification.
- [x] Record production removals, retained boundaries and remaining limitations.

Keep Qt Widgets/Windows behavior, one privilege request per job, existing source
and system-device protections, cancellation cleanup, complete sync/readback and
helper exit before successful READY. Windows validation remains deferred.

Evidence and pre-existing work are preserved in
`.rufus-test-artifacts/consolidation-baseline.tar.gz` and
`consolidation-runtime-baseline.json`; no clean-tree assumption or reset is used.

## Result

The main problem was duplicated execution ownership and repeated planning,
rather than a wholesale replacement of Rufus's algorithms. This pass removes
that duplication without introducing a general backend framework, Wine-style
API surface or new template/callback hierarchy. Existing shared Rufus algorithms
and native tools remain the foundation. No Windows source or build file changed
relative to this pass's preserved baseline.

| Production measure | Before | After |
| --- | ---: | ---: |
| Unique Linux `.c`/`.cpp` lines in GUI/helper input graphs | 14,644 | 14,332 |
| GUI graph, including common library implementation files | 13,757 | 12,295 |
| Helper graph, including common library implementation files | 10,806 | 10,739 |
| GUI DD execution routes | 2 | 1 |

The unique reduction is 312 lines (2.1%). The larger GUI graph reduction is
mostly moving helper-only responsibilities out of its dependency graph; it must
not be added to the unique reduction. These are physical source lines, including
comments/blanks, from production Ninja input graphs, not binary-size measurements
or exact counts of reachable functions. Tests, probes, headers, generated
resources, build scripts, vendored upstream sources and unlinked adapters are
excluded. The native Syslinux executable uses unchanged upstream implementation
files and generated payload arrays; this pass adds no installer implementation.
The before/after JSON inventories make the counting scope reproducible.

## Changes and reviewed boundaries

| Area | Decision |
| --- | --- |
| Target descriptor validation | `InspectWritableTarget` owns type, access mode, incompatible flags, dev_t, size, read-only state and sector inspection. Partition, FAT, NTFS, exFAT and raw writers retain their extent/geometry and filesystem checks. It grants no authorization or exclusive claim. |
| DD execution | Removed `RawWriteJob`, local-worker lifetime/result handling, UDisks descriptor acquisition/transfer and dual-route selection. The existing helper performs all writes and cleanup. `UDisksTarget` remains an asynchronous read-only preflight suitable for the GUI. |
| GUI/helper separation | Moved read-only selection and disk-sequence checks into `device_selection`. `nm -C --defined-only` confirms the GUI no longer contains `PrepareDeviceTarget`, its mount/formatter methods, or `WriteRawDevice`. Low-level formatter code can still share an object with option validators; this is not a claim that every write-related symbol disappeared. |
| Format planning | Cluster filtering, ISO/non-bootable preflight and helper preparation call `PlanFormatPartition`; preflight shares `CheckFormatSettings`. One helper-local operation selects GPT/MBR/SFD creation. Original Rufus layout arithmetic and native libfdisk serialization remain in use. The helper recomputes and checks its target before writes. |
| Process protocol | One expected phase list defines both accepted phase order and the final successful phase. Raw counters retain their separate copy/sync/verify contract. |
| Format workflows | Keep FAT/exFAT claim handoff distinct from NTFS's retained descriptor, UEFI:NTFS installation, and FUSE cleanup. Merging these into a generic job/formatter framework would obscure different lifetimes. Small repeated outcome assignments remain explicit. |
| Extraction and file adapters | Keep the existing common ISO/UDF walker, filename/config rules and extraction/preflight engine. Native directory traversal, safe replacement, cancellation, full sync and filesystem-specific readback have concrete roles; they are not duplicate Win32 UI logic. No new generic file-I/O layer was added. |
| Tool and WIM/Windows support | Keep one existing child-command runner for formatting/installation, upstream native Syslinux, upstream wimlib plus its small reader extension, and Hivex. exFAT capability probing differs from executable-presence checks for a reason. Shared unattended/configuration rules remain shared; platform file/registry adapters remain explicit. |
| Qt controllers and forms | Preserve separate DD and ISO interaction flows: ISO has mode selection, image/settings preflight and Windows options. They share the helper client. No form/UI hierarchy rewrite is warranted for a Widgets port. |

Fresh source/device checks on both sides of authorization and after operations
are retained. They validate different moments and trust boundaries; removing
those calls to improve line counts would weaken the job. Common implementations
avoid rules drifting without treating a prior GUI check as root authorization.

## Deliberate behavior change

All creation now requires the matching helper, including unmounted DD. Earlier
CP17a/CP17b7/CP17d notes requiring helper-independent DD are historical and
superseded. One helper avoids two authorization, cancellation and completion
lifecycles, and prevents device mount state or the fixed-USB checkbox from
changing the writer architecture. It also provides the consistent route needed
for future fixed-media classification. There is no silent fallback or second
prompt when acquisition fails.

All DD sources now obey the existing helper source contract: caller-owned,
owner-readable regular files with retained identity checks. This can reject a
readable image owned by another account that the former unprivileged DD worker
could open. This restriction prevents the root helper from reading a source
with privileges unavailable to its caller; supporting other ownership/ACL cases
would need a separately reviewed source-access design.

The usual ISO/DD choice and recommended ISO default, raw percentages,
confirmation warnings, cancellation flush, verification and exit-before-READY
remain. The helper accepts DD's existing power-of-two sector range from 512 to
65536 bytes with aligned image sizes. Formatting remains restricted to 512-byte
sectors. No automatic eject or read/write remount was added.

## Qualification and evidence

- Normal full build and 39 affected CTest suites passed, including filesystem
  and layout variants, helper/process protocol, source checks, UDisks and Widgets.
- ASan/UBSan with leak detection: full build and 24 affected suites passed.
- After final descriptor/diagnostic cleanup, all 11 affected suites passed again
  normally and with sanitizers. Two optional real-ISO GUI rows remain unexercised;
  generated-image regressions cover this pass. No further Windows customization
  qualification was performed.
- Generated USB VM: 13 SFD/FAT32 handoff/error/cancellation cases passed, with
  independent FAT readback. 4K formatting was refused without changing the disk;
  raw helper acquisition of that 4K device succeeded. This is acquisition coverage,
  not a complete 4K DD or mounted-4K qualification.
- Generated USB VM: five production Syslinux helper jobs and five mounted/unmounted
  DD client/helper jobs passed. These cover changed source, real EBUSY, cancellation
  after unmount, one launcher per job, full counters, delayed exit, independent
  disk/filesystem readback and BIOS boot. Repeated successfully with the final
  helper binary. The VM uses a guest-only pkexec stand-in; a real desktop prompt
  was not requalified in this pass.

All VMs used TCG, an installed 6.18.52 kernel and generated regular-file disks,
with no host block devices, USB passthrough or networking. Windows validation
remains deferred. VM targets were restored OFF. Disposable guest trees/disks were
removed; only small manifests, hashes and logs were retained.

Evidence is under `.rufus-test-artifacts/consolidation-*`: baseline archive,
scoped patch/source hashes, production inventories, normal/sanitizer logs and
VM result directories. A transient linker failure passed on rebuild; later
sandbox launcher stalls were diagnosed and the workspace builds/tests completed
through approved escalated calls. No permission rule was changed, and no test
failure was hidden or disabled. No commit, host installation or physical USB
write was performed.

## Next work

This consolidation is complete. Resume the existing CP17d5c score-input/card
policy checkpoint when feature work resumes. Keep new reusable image/policy
rules in the common Rufus layer and native operations in these concrete adapters;
extend the existing helper job instead of adding another execution route.
