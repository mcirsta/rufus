# Windows parity completion audit — 2026-10-23

## Current release work — CP77 / Preview46 (2026-10-24)

**B1–B5 are delivered. CP77 qualifies the remaining supported-UDF release policy.**
Alt+Q survives filesystem changes and freezes with the UDF job. Disabling indexing
reports the native provider's optional limitation; formatting and existing files
continue through the normal sync/cleanup/helper-exit gate. **The Windows UDF
indexing attribute is not written on disk.** See the
[metadata policy and evidence](linux-filesystem-metadata-policy.md).

The user explicitly deferred UDF 2.50/2.60 (B6/I14), ReFS formatting (I15) and
Dev Drive detection (I10). Existing UDF 1.02–2.01 support remains. There is no
blanket ReFS virtual-disk refusal or substitute Dev Drive detector. Full Windows
parity remains incomplete; A03 retains its actual metadata gap in the register.

**The useful-release work now proceeds to B8:** full native translations (I05),
consolidated evidence (Q01), bounded host/desktop/physical-media checks (Q02),
Debian/RPM install/upgrade/removal qualification (Q03), and reviewable versioned
release/update assets (Q04). Existing Arch/generic x86-64 delivery remains the
qualified starting point. No additional host/package scope reduction was agreed.
Publication requires authorization after the assets are ready.

Three grouped component suites pass 163 substantive cases in each normal/sanitizer build, with zero failures/skips; two focused UDF selection rows pass in each build. One combined native route per build qualifies two ordinary UDF jobs (GPT/SFD, indexing disabled/enabled), exact optional diagnostics, saved/frozen preferences, independent Unicode label/icon/ZIP readback, one authorization and sync/cleanup/helper exit before full READY. One full nine-suite Qt gate passes 2,217 substantive cases with 2 existing optional skips and zero failures. Unchanged CP76 geometry, writer/helper faults and boot evidence are reused.

The source comparison below is the **audit snapshot before CP72**. Its paused-goal,
missing-control and original-count statements describe that snapshot. Development
was subsequently resumed by the user; the source hash manifest records the old
audit baseline, not a claim that delivered CP72/CP73 sources match CP71.


## Result and fixed scope

The baseline is **Windows Rufus 4.15.2404 in this checkout versus Linux CP71 /
Preview40**. This is a source and retained-evidence audit, not a comparison with
the latest upstream release. The inherited working tree is part of the baseline;
the Git commit alone does not identify these sources.

The register has **25 implementation work packages, four native adaptation
decisions and four qualification/release packages**, assigned exactly once to
**eight completion batches**. The packages are acceptance lists inside a batch,
not separate checkpoints or equal units of effort. There is no defensible
percentage or completion date from these counts. ReFS and UDF metadata partitions
are substantial provider projects and dominate the uncertainty.

The complete Windows feature scope remains: SFD, legacy and expert features are
included. An unavailable provider does not remove a feature. Full parity is not
complete. The development goal remains **paused** while this agreed audit is
performed; no feature development, build, test, guest, host-media action or
publication was performed for this audit.

The [machine-readable register](linux-parity-register.json) holds every item,
source anchor, dependency, acceptance condition and coverage disposition. The
[delivery plan](linux-release-plan.md) defines batch gates. This document and the
register supersede broad “remaining combinations” language in historical
checkpoint notes.

## Eight completion batches

| Batch | Deliverable | Register entries |
| --- | --- | --- |
| B1 | Desktop controls and native messages | [I01](linux-parity-audit.md#i01), [I02](linux-parity-audit.md#i02), [I03](linux-parity-audit.md#i03), [I04](linux-parity-audit.md#i04), [I05](linux-parity-audit.md#i05) |
| B2 | Device and source operations | [I06](linux-parity-audit.md#i06), [I07](linux-parity-audit.md#i07), [I08](linux-parity-audit.md#i08), [I09](linux-parity-audit.md#i09), [I11](linux-parity-audit.md#i11), [A01](linux-parity-audit.md#a01), [A04](linux-parity-audit.md#a04) |
| B3 | ISO extraction, EFI and boot resources | [I16](linux-parity-audit.md#i16), [I20](linux-parity-audit.md#i20), [I21](linux-parity-audit.md#i21), [I22](linux-parity-audit.md#i22), [I23](linux-parity-audit.md#i23), [I24](linux-parity-audit.md#i24), [I25](linux-parity-audit.md#i25) |
| B4 | Legacy Windows, ReactOS and Kolibri ISO routes | [I17](linux-parity-audit.md#i17), [I18](linux-parity-audit.md#i18), [I19](linux-parity-audit.md#i19) |
| B5 | Sector and filesystem geometry | [I12](linux-parity-audit.md#i12), [I13](linux-parity-audit.md#i13), [A02](linux-parity-audit.md#a02) |
| B6 | UDF metadata-partition provider | [I14](linux-parity-audit.md#i14) |
| B7 | ReFS provider and filesystem metadata policy | [I10](linux-parity-audit.md#i10), [I15](linux-parity-audit.md#i15), [A03](linux-parity-audit.md#a03) |
| B8 | Consolidated release qualification | [Q01](linux-parity-audit.md#q01), [Q02](linux-parity-audit.md#q02), [Q03](linux-parity-audit.md#q03), [Q04](linux-parity-audit.md#q04) |

B1 is the next implementation batch when development resumes. B2 and B3 do not
require B1; B4 consumes B3, B5 unlocks B6/B7, and B8 follows all feature batches.
The provider work can be investigated earlier, but does not justify splitting
ordinary features into source/helper/guest/GUI checkpoints.

## Confirmed implementation work

These are source-level refusals or missing dispatch/settings paths. They do not
mean the delivered foundation for that area needs reimplementation.

### I01

Delivered by [CP72](linux-desktop-controls.md). The following finding is historical.

**Cache, priority, USB diagnostics and log keyboard action — B1.** Alt+D, Alt plus/minus and Alt+. have no native actions; DefaultThreadPriority and EnableUsbDebug are not consumed. Windows Ctrl+A selects a visible log even when another window owns focus; native standard editor selection is focus-local.

Close when: Provide all three actions with original session/saved behavior and idle/active guards. Priority must affect actual workers through a documented native scheduling mapping; diagnostics must affect enumeration/logging. Delete only owned downloaded resources, invalidate retained cache selections and preserve sources, settings and logs. Match visible-log Ctrl+A routing without stealing ordinary text selection when the log is closed.

Windows: [src/rufus.c:3824](../src/rufus.c#L3824), [src/rufus.c:3767](../src/rufus.c#L3767), [src/rufus.c:3784](../src/rufus.c#L3784), [src/rufus.c:3718](../src/rufus.c#L3718). Native evidence: [linux/qt/main_window.cpp:92](../linux/qt/main_window.cpp#L92), [linux/qt/application_storage.cpp:54](../linux/qt/application_storage.cpp#L54).

### I02

Delivered by [CP72](linux-desktop-controls.md). The following finding is historical.

**Explicit dark appearance override — B1.** The desktop theme is used, but the saved Ctrl+Alt+D user override is absent.

Close when: Offer and persist an explicit light/dark override with coherent restart/recreation behavior, readable controls and unchanged active-job completion.

Windows: [src/rufus.c:4022](../src/rufus.c#L4022), [src/settings.h:36](../src/settings.h#L36). Native evidence: [linux/qt/main_window.cpp:92](../linux/qt/main_window.cpp#L92).

### I03

Delivered by [CP72](linux-desktop-controls.md). The following finding is historical.

**Progress display selection — B1.** Bare Alt does not cycle percentage, rate and remaining time in the native window.

Close when: Cycle the three original views when measurements exist; retain unavailable-value behavior, menu shortcuts, elapsed clock and READY-after-exit ordering.

Windows: [src/rufus.c:4069](../src/rufus.c#L4069). Native evidence: [linux/qt/main_window.cpp:92](../linux/qt/main_window.cpp#L92).

### I04

Delivered by [CP72](linux-desktop-controls.md). The following finding is historical.

**Advanced application update diagnostics and force mode — B1.** Ordinary Alt+Y exists. Ctrl+Alt+Y timestamp-ignore force mode and the verbose update setting have no native counterpart.

Close when: Implement the explicit advanced force mode and meaningful verbose diagnostics for the native release service; retain consent, channel, origin and architecture checks. Invalid dates in ordinary mode must still preserve the last successful timestamp.

Windows: [src/rufus.c:4060](../src/rufus.c#L4060), [src/settings.h:62](../src/settings.h#L62). Native evidence: [linux/qt/main_window.cpp:409](../linux/qt/main_window.cpp#L409).

### I05

**Native user-facing translation coverage — B8 (user scheduling decision 2026-10-23).** The original 38-language selector and reused messages work. Native-only user-facing diagnostics, filters and unmatched templates still fall back to English.

Close when: Freeze an inventory of reachable native user-facing strings, reuse original message IDs where equivalent and supply reviewed native translations for the declared languages. Record original catalog inheritance/fallback separately. Check placeholders, plural forms, filters, RTL and live cached dialogs. Do not translate technical trace logs merely because Windows also logs them in English.

Windows: [src/localization.c:36](../src/localization.c#L36), [src/settings.h:53](../src/settings.h#L53). Native evidence: [docs/linux-localization.md:49](../docs/linux-localization.md#L49).

### I06

**Raw optical disc capture — B2.** USB filesystem-to-UDF ISO export is delivered; Alt+O raw optical media capture is a distinct missing source workflow.

Close when: Discover readable optical media, choose the original applicable disc source, save exact device bytes transactionally, support progress/cancel/read failures and release all read handles before successful completion. Reuse capture destination publication; do not mount or rewrite the source.

Windows: [src/iso.c:1396](../src/iso.c#L1396), [src/rufus.c:3918](../src/rufus.c#L3918). Native evidence: [linux/platform/udisks_client.cpp:71](../linux/platform/udisks_client.cpp#L71), [linux/platform/iso_export.cpp:177](../linux/platform/iso_export.cpp#L177).

### I07

**USB port reset and GPT ESP type toggle — B2.** Alt+C device port cycling and Alt+P ESP-to-data/original-type restoration have no native command path.

Close when: Provide native selected-device reset/re-enumeration and ESP type toggling, including original ToggleEsp GUID restoration records. Freeze and revalidate identity, preserve unrelated partitions/bytes and use one authorization, final sync/cleanup and correct refresh. No port action may reset an unrelated device.

Windows: [src/dev.c:132](../src/dev.c#L132), [src/drive.c:1551](../src/drive.c#L1551), [src/rufus.c:3923](../src/rufus.c#L3923). Native evidence: [linux/platform/prepare_device.cpp:91](../linux/platform/prepare_device.cpp#L91), [linux/qt/main_window.cpp:92](../linux/qt/main_window.cpp#L92).

### I08

**Configured device exclusion tables — B2.** Eight IgnoreUsb VID/PID slots and eight IgnoreDisk decorated-GUID slots are not read natively.

Close when: Read the original numbered keys and match USB VID/PID and GPT disk GUID exclusions before listing/writing. Preserve system/application/source protection, explicit advanced class choices, malformed-value diagnostics and settings reset/import behavior.

Windows: [src/dev.c:584](../src/dev.c#L584), [src/drive.c:2681](../src/drive.c#L2681), [src/rufus.h:95](../src/rufus.h#L95). Native evidence: [linux/platform/udisks_client.cpp:143](../linux/platform/udisks_client.cpp#L143), [linux/qt/application_storage.cpp:54](../linux/qt/application_storage.cpp#L54).

### I09

**File-backed virtual devices and drive-less UDisks objects — B2.** CP71 covers recognized native SCSI virtual/VMware, MMC and removable devices. Loop/NBD and eligible drive-less virtual objects remain refused.

Close when: Support positively identified file-backed loop and NBD virtual disks and their drive-less discovery records under the existing explicit virtual-device option. Resolve backing storage, retain class identity through the helper and refuse running-system/application/source disks. Reuse the writer; qualify one route per distinct native class, not per virtual-disk vendor.

Windows: [src/dev.c:49](../src/dev.c#L49). Native evidence: [linux/platform/udisks_client.cpp:343](../linux/platform/udisks_client.cpp#L343), [linux/platform/native_disk_facts.cpp:158](../linux/platform/native_disk_facts.cpp#L158).

### I10

**Deferred by the user after the CP77 provider audit (2026-10-24).** Actual Dev
Drive recognition remains unimplemented; no blanket ReFS refusal is substituted.
See the [metadata policy](linux-filesystem-metadata-policy.md). Earlier acceptance
conditions below remain for future resumption.

**Microsoft Dev Drive exclusion — B7.** Sandbox/antivirus exclusions exist; the Windows Dev Drive volume-state exclusion has no native metadata probe.

Close when: Recognize the actual on-disk Dev Drive state using a bounded read-only native provider and exclude it under the original virtual-drive rule. Prove positive, negative and unavailable/corrupt-metadata behavior. Do not guess from a volume label.

Dependency correction after the ReFS formatter deferral: this protection needs
a bounded read-only metadata provider, not a working formatter. I10 remains
active independently of I15.

Windows: [src/drive.c:71](../src/drive.c#L71). Native evidence: [linux/platform/udisks_client.cpp:405](../linux/platform/udisks_client.cpp#L405).

### I11

**Mounted source and caller-home backing resolution — B2.** Direct block/fuseblk and memory sources work. The shared source/home check refuses loop-backed and unresolved anonymous mounts; CP65 application backing resolution has broader support that is not reused here.

Close when: Resolve Btrfs member disks, file-backed loop and stacked block relationships for source/home protection and automatic unmount. Support positively resolvable overlay/FUSE backing and positively identified nonlocal sources; retain explicit refusal for opaque unresolved backing. Keep every parent/container/supplementary source protected, with one helper and no prewrite changes on refusal.

Windows: [src/drive.c:2659](../src/drive.c#L2659). Native evidence: [linux/platform/target_mounts.cpp:148](../linux/platform/target_mounts.cpp#L148), [linux/platform/application_devices.cpp:83](../linux/platform/application_devices.cpp#L83).

### I12

**Logical 4-KiB formatting geometry — B5.** Raw writing already accepts its native sector range. Formatting/layout/provider options are hard-coded to 512 logical bytes and reject 4Kn.

Close when: Carry real logical/physical sector geometry through discovery, requests, plans, all eligible filesystem providers, boot records and readback. Qualify 512/512e and 4096 logical-byte cases at actual policy boundaries on MBR/GPT/SFD; preserve native raw support and explicit filesystem/boot limits.

Windows: [src/rufus.c:454](../src/rufus.c#L454), [src/drive.c:2278](../src/drive.c#L2278). Native evidence: [linux/platform/prepare_device.cpp:166](../linux/platform/prepare_device.cpp#L166), [linux/platform/format_options.cpp:18](../linux/platform/format_options.cpp#L18).

### I13

**UDF block size and volume geometry — B5.** The native UDF writer and verifier use 512-byte blocks and a 32-bit block-count limit, restricting volumes to approximately 2 TiB.

Close when: Choose and verify 512/1024/2048/4096 UDF logical blocks where allowed by real device/provider geometry; separate UDF block size from device sector size. Cover transitions and bounded descriptor/anchor arithmetic. Establish the actual maximum supported by the Windows formatter/provider, rather than claiming all volumes under its 256-TiB UI cap necessarily format.

Dependencies: I12.

Windows: [src/rufus.c:469](../src/rufus.c#L469). Native evidence: [linux/platform/udf_format.cpp:158](../linux/platform/udf_format.cpp#L158).

### I14

**Deferred by user decision 2026-10-24.** Keep UDF 1.02–2.01; disclose the
[newer-revision gap](linux-filesystem-formatting.md#deferred-udf-250260-gap--user-decision-2026-10-24).
The acceptance conditions below remain for a future resumption. This item is
unimplemented, not delivered; useful-release preparation does not wait for it.

**UDF 2.50 and 2.60 metadata partitions — B6.** The integrated udftools 2.3 path implements revisions 1.02, 1.50, 2.00 and 2.01; metadata-partition revisions are absent.

Close when: Deliver a native formatter/provider for actual 2.50 and 2.60 metadata-partition media, independent descriptor/readback verification, revision selection and original eligible layouts. Include full/quick, cancel, label and completion semantics; never silently substitute a lower revision. Audit provider licensing/maintenance and concrete on-disk compatibility first.

Dependencies: I13.

Windows: [src/settings.h:57](../src/settings.h#L57), [src/format.c:466](../src/format.c#L466). Native evidence: [docs/linux-filesystem-formatting.md:47](../docs/linux-filesystem-formatting.md#L47).

### I15

**Excluded from first-release requirements by the user (2026-10-24): "remove
just ReFS".** The unimplemented formatter remains a disclosed full-parity gap.
The user subsequently also deferred Dev Drive detection. CP77 qualifies the
supported-UDF optional-provider limitation; its actual metadata gap stays open.
Full native translations remain active. Acceptance below is retained for future resumption.

**Native ReFS formatting provider — B7.** No native ReFS formatting case/provider is integrated. Existing provider investigation did not identify an integrated formatter.

Close when: Deliver a licensed native formatter with independent metadata/readback checks and matching applicable fixed-media, capacity and 4/64-KiB cluster eligibility. Establish actual Windows provider revision/capacity support; add MBR/GPT/SFD and normal helper/quick/full/cancel/label behavior where offered. An unavailable provider remains an open feature, not a completed limitation.

Dependencies: I12.

Windows: [src/rufus.c:370](../src/rufus.c#L370), [src/rufus.c:483](../src/rufus.c#L483). Native evidence: [linux/platform/format_options.cpp:191](../linux/platform/format_options.cpp#L191), [docs/linux-filesystem-formatting.md:53](../docs/linux-filesystem-formatting.md#L53).

### I16

**Source-selected EFI architectures — B3.** Ordinary ISO creation preflights only bootx64.efi/PE32+. The shared scanner recognizes eight EFI architecture names.

Close when: Admit and validate original-eligible ia32, x64, arm, aa64, ia64, riscv64, loongarch64 and ebc source fallback applications with correct machine/format checks and source names. Keep selection based on source, not host. Reuse existing generic UEFI:NTFS/runtime resources; preserve IA64/EBC without inventing missing Windows validator binaries. Windows-specific repair/customization must retain its actual architecture eligibility.

Windows: [src/image_file.c:111](../src/image_file.c#L111). Native evidence: [linux/platform/iso_write_check.cpp:479](../linux/platform/iso_write_check.cpp#L479), [linux/platform/iso_write_check.cpp:493](../linux/platform/iso_write_check.cpp#L493).

### I17

**Windows and legacy FAT16 boot routes — B4.** Native Windows BIOS/dual admits FAT32 or NTFS; older UEFI explicitly refuses FAT16, and legacy WinPE FAT16 remains gated.

Close when: Reproduce Windows-eligible FAT16 BIOS/dual and older UEFI source paths using exact original PBR/loader expectations. Cover normalized Windows 7/8/8.1/10/11 and legacy i386/amd64/minint only where Windows actually offers them. Verify copied bytes, generated loader handoff and source restrictions; do not schedule installed-Windows/customization tests.

Dependencies: I18.

Windows: [src/format.c:912](../src/format.c#L912), [src/format_policy.c:148](../src/format_policy.c#L148). Native evidence: [linux/platform/iso_write_check.cpp:378](../linux/platform/iso_write_check.cpp#L378), [linux/platform/iso_write_check.cpp:222](../linux/platform/iso_write_check.cpp#L222).

### I18

**Older BOOTMGR, PE and mixed-loader dispatch — B4.** The known-installer predicate restricts releases to 7/8/8.1/10/11. Existing WinPE preparation works for its qualified families but rejects BOOTMGR-EFI/install-archive mixtures and other original dispatch cases.

Close when: Handle the finite original source classes: older normalized-unknown BOOTMGR installers (including Vista), BOOTMGR/WIM PE without an install archive, and i386/amd64/minint with the additional BOOTMGR/EFI/install-archive or loader combinations selected by original GetBootInstallType/GetSecondaryBootType. Preserve existing SIF encodings, loader ownership and NTFS repair requirements; compare representative dispatch/bytes without a per-release OS-install matrix.

Dependencies: I16, I20, I22.

Windows: [src/format.c:1900](../src/format.c#L1900), [src/wue.c:175](../src/wue.c#L175). Native evidence: [linux/platform/iso_write_check.cpp:357](../linux/platform/iso_write_check.cpp#L357), [linux/platform/iso_write_check.cpp:230](../linux/platform/iso_write_check.cpp#L230).

### I19

**ReactOS and KolibriOS ISO creation — B4.** Standalone ReactOS is delivered. ReactOS ISO and KolibriOS ISO are explicitly refused by ordinary native creation.

Close when: Deliver both original ISO dispatch routes: ReactOS loader/Syslinux selection and freeldr/setupldr files; Kolibri FAT32/PBR with its root MTLD_F32 loader and actual hidden/system attributes. Retain original FAT16 refusal where applicable and native authorization/sync/cleanup/boot handoff.

Dependencies: I22.

Windows: [src/format_policy.c:56](../src/format_policy.c#L56), [src/format.c:1384](../src/format.c#L1384), [src/format_policy.c:64](../src/format_policy.c#L64). Native evidence: [linux/platform/iso_write_check.cpp:380](../linux/platform/iso_write_check.cpp#L380).

### I20

**Embedded FAT EFI extraction and broken-link repair — B3.** Embedded EFI FAT inspection exists, but extraction/fallback repair and security enumeration of those files are not integrated into the native ISO write path.

Close when: Extract the original eligible embedded EFI tree and replace a proven broken fallback link as Windows does. Validate paths, machine type and retained source before authorization, include embedded EFI binaries in security analysis and final checksum/space planning, and preserve regular source fallback precedence.

Dependencies: I16.

Windows: [src/iso.c:1011](../src/iso.c#L1011), [src/pki.c:351](../src/pki.c#L351). Native evidence: [linux/platform/iso_write_check.cpp:404](../linux/platform/iso_write_check.cpp#L404), [linux/platform/secure_boot.cpp:18](../linux/platform/secure_boot.cpp#L18).

### I21

**ISO symbolic links and nonregular-entry compatibility — B3.** The native extractor rejects Rock Ridge symlinks and UDF nonregular entries that Windows processes.

Close when: Reproduce resolved file/directory links as Windows-compatible NTFS reparse links; implement FAT syslinux-to-isolinux materialization, /firmware target duplication and original literal-target/skip rules. Match Windows UDF entry-as-file behavior for its supported link cases. Resolve only within the retained image/output tree; never follow source-controlled links into the host. Include checksum and companion configuration updates.

Windows: [src/iso.c:662](../src/iso.c#L662), [src/iso.c:106](../src/iso.c#L106), [src/iso.c:451](../src/iso.c#L451). Native evidence: [linux/platform/iso_extract.c:645](../linux/platform/iso_extract.c#L645).

### I22

**Image-matched Syslinux versions and obsolete modules — B3.** The ISO installer is pinned to the qualified 6.04-pre1 resources. Windows selects source-matched ldlinux.sys/bss and can replace old menu.c32/vesamenu.c32.

Close when: Support Windows-accepted Syslinux major families 3/4/5/6 by their actual version/resource contract, full suffix then original base fallback. Implement signed/cache/download/cancel handling for selected companion pairs and the two obsolete COM32 modules with original thresholds. Reject unknown incompatible payloads explicitly; avoid a per-distribution or every-release test matrix.

Windows: [src/rufus.c:104](../src/rufus.c#L104), [src/rufus.c:1311](../src/rufus.c#L1311), [src/image_report.h:41](../src/image_report.h#L41). Native evidence: [linux/platform/syslinux_native.cpp:51](../linux/platform/syslinux_native.cpp#L51), [linux/platform/syslinux_source.cpp:62](../linux/platform/syslinux_source.cpp#L62).

### I23

**Slax relocation and missing-directory ISO recovery — B3.** The native ISO copy path lacks the original Slax EFI relocation and recovery when an ISO omits parent-directory entries.

Close when: Move /slax/boot/EFI to /EFI only under original absence/precedence rules, retain accompanying Syslinux config selection, and create missing safe parents for the exact Windows-supported mastering fault. Prove generated positive/refusal cases and final checksum updates; never accept traversal, absolute paths or conflicting aliases.

Windows: [src/iso.c:1077](../src/iso.c#L1077), [src/iso.c:721](../src/iso.c#L721). Native evidence: [linux/platform/iso_extract.c:110](../linux/platform/iso_extract.c#L110), [linux/platform/config_patch.cpp:160](../linux/platform/config_patch.cpp#L160).

### I24

**Syslinux v4 host portability — B3.** The native Syslinux 4.07 build explicitly stops on non-x86 hosts because its unmodified non-x86 accessors are unsuitable.

Close when: Port the original v4 accessor/install path to alignment-safe endian-correct native C on aarch64 while preserving x86 bytes. Keep upstream provenance, provider build/install registration and representative installer/readback checks. This is a host-tool gap, separate from image EFI architecture.

Windows: [rufus.sln:42](../rufus.sln#L42). Native evidence: [linux/syslinux/CMakeLists.txt:10](../linux/syslinux/CMakeLists.txt#L10).

### I25

**Secure Boot issuer lookup outside embedded certificates — B3.** Windows cache-only chain lookup may find an immediate issuer outside the signed image. Native lookup currently uses the embedded certificate collection only.

Close when: Add bounded native cached/system issuer lookup with deterministic embedded-versus-cache precedence and the original signer fallback. Preserve issuer metadata classification rather than treating successful chain construction as publisher authentication. No live AIA/network fetch is required; cover missing, ambiguous and unavailable issuers.

Windows: [src/pki.c:409](../src/pki.c#L409). Native evidence: [linux/platform/secure_boot.cpp:154](../linux/platform/secure_boot.cpp#L154).

## Native adaptation decisions

### A01

**Drive-locking override — B2.** Windows exposes a session opt-out of physical-drive locking; the native helper requires an exclusive claim.

Close when: Implement a meaningful explicit native opt-out retaining identity, source protection and sync/cleanup, or document and demonstrate the concrete Linux requirement preventing it. An inert toggle or generic safety preference does not close this row; any unavoidable divergence must satisfy AGENTS.md.

Windows: [src/rufus.c:3793](../src/rufus.c#L3793). Native evidence: [linux/platform/prepare_device.cpp:53](../linux/platform/prepare_device.cpp#L53).

### A02

**Forced FAT32 formatter and Windows VDS backend controls — B5.** Linux already uses one native FAT32 formatter at every size and the agreed FAT32 label. Separate Windows FormatEx/large-FAT/VDS switches are absent.

Close when: Compare original observable cluster, quick-format, full-format and eligibility effects of ForceLargeFat32Formatting and UseVds against the native path. Implement missing effects or document a tested coherent single-provider equivalent. Do not reproduce Windows API selection with a no-op control.

Windows: [src/rufus.c:3896](../src/rufus.c#L3896), [src/rufus.c:3970](../src/rufus.c#L3970). Native evidence: [linux/platform/fat_format.c:69](../linux/platform/fat_format.c#L69), [docs/linux-gui-parity.md:40](../docs/linux-gui-parity.md#L40).

### A03

**CP77 release policy qualified; actual metadata parity remains open.** The saved
choice is frozen for supported UDF; the unavailable optional attribute reports a
nonfatal diagnostic. No Windows UDF indexing flag is written. See the
[policy/evidence](linux-filesystem-metadata-policy.md). The full-parity acceptance
conditions below are retained, not declared satisfied.

**UDF/ReFS file indexing hint — B7.** CP70 implements the actual NTFS root indexing flag. The equivalent UDF/ReFS on-disk hint is not handled.

Close when: Implement each provider-supported Windows on-disk hint and inspect it independently, or prove the original provider does not support it and reproduce its optional-failure diagnostic/eligibility. Desktop search preferences alone are not an equivalent. Preserve saved Alt+Q state across filesystem changes.

Dependencies for eventual full coverage: I14, I15. The existing-UDF hint remains
active independently; the newer-UDF/ReFS parts follow their deferred providers.

Windows: [src/format.c:1768](../src/format.c#L1768). Native evidence: [docs/linux-ntfs-options.md:66](../docs/linux-ntfs-options.md#L66).

### A04

**Windows-specific virtual-disk parent locators — B2.** Relative VHD/VHDX chains and immutable log replay are delivered. Drive/UNC-only locator resolution and parent_linkage2 are explicitly unsupported.

Close when: Determine the exact Windows desktop-supported alternate locator/linkage cases. Provide explicit native parent/path mapping or relocation input for original-supported nonportable paths, validating parent IDs, geometry and timestamps and retaining every source before authorization. Do not infer Windows drive letters from Linux paths or rewrite the source container. Record any format-defined locator case the desktop API itself does not accept.

Windows: [src/vhd.c:385](../src/vhd.c#L385). Native evidence: [linux/platform/virtual_disk.c:114](../linux/platform/virtual_disk.c#L114), [linux/platform/compressed_vhdx.c:398](../linux/platform/compressed_vhdx.c#L398).

## Qualification and release work

### Q01

**Consolidated eligible-media and firmware coverage — B8.** Most media behavior already has component, native guest/readback and representative firmware evidence; additional sources/geometry above need one final reconciliation.

Close when: Produce one evidence manifest for the 10 boot choices, the 8 actually selectable filesystems, MBR/GPT/SFD, BIOS/UEFI, original expert/advanced flags and 512/4Kn boundaries. Each policy-eligible case must cite existing unchanged evidence, a new shared-path case or its owning implementation/adaptation row. Qualify implemented non-x64 To Go source bytes/EFI handoff where applicable. Do not multiply invalid Cartesian combinations or run more installed-Windows/customization tests.

Windows: [src/format_policy.c:148](../src/format_policy.c#L148), [src/rufus.c:191](../src/rufus.c#L191). Native evidence: [docs/testing-and-checkpoints.md:14](../docs/testing-and-checkpoints.md#L14).

### Q02

**Physical media, supported hosts and desktop qualification — B8.** x86-64 simulated media and Qt evidence are extensive; physical unplug durability and additional host/desktop configurations are not fully qualified.

Close when: Use one representative physical USB for write/readback, immediate unplug after joined READY and reconnect; one native 4Kn device if available; bounded hotplug/desktop-automount contention and cancellation/recovery. Build/run representative i686 and aarch64 Linux hosts in addition to x86-64. Exercise one X11 and one Wayland desktop, normal/enlarged DPI, RTL and keyboard/accessibility once. Record unavailable hardware honestly; no USB-model/distro-version matrix.

Dependencies: I12, I24.

Windows: [rufus.sln:42](../rufus.sln#L42), [src/rufus.c:957](../src/rufus.c#L957). Native evidence: [docs/linux-support.md:1](../docs/linux-support.md#L1).

### Q03

**Distribution packages and dependency delivery — B8.** Arch/CachyOS and generic archive packages are delivered; Debian/RPM dependency/install qualification remains separate from media algorithms.

Close when: Deliver/inspect one Debian-family package and one RPM-family package alongside existing Arch/generic delivery. Check fresh install, helper/polkit/resource paths, required provider/runtime versions, upgrade and removal on one representative environment each. Record supported host/package combinations, rather than promising every distribution.

Windows: [src/rufus.c:3157](../src/rufus.c#L3157). Native evidence: [linux/packaging/PKGBUILD.in:3](../linux/packaging/PKGBUILD.in#L3), [linux/CMakeLists.txt:746](../linux/CMakeLists.txt#L746).

### Q04

**Versioned release publication and native update handoff — B8.** Native release checks, consent, scheduling and package-manager handoff are implemented. A published compatible native release/update path needs release evidence; CP68 recorded an empty-catalog snapshot on 2026-10-21, not a current live assertion.

Close when: Prepare versioned notes and supported-boundary documentation, package manifests and distribution verification/update instructions; publish only with authorization. Then qualify one actual publisher release listing, supported asset selection and explicit release-page/package handoff. Preserve signed external boot-resource verification. Deferred original Windows execution is disclosed, not turned into a new installation test gate.

Dependencies: Q01, Q02, Q03.

Windows: [src/stdlg.c:1925](../src/stdlg.c#L1925). Native evidence: [docs/linux-application-updates.md:6](../docs/linux-application-updates.md#L6).

## Delivered foundations retained

A closed foundation can still have a specific extension above. For example,
relative parent chains are delivered while nonportable Windows locator handling
is A04. ISO filesystem export is delivered while optical raw capture is I06.

| ID | Delivered work | Retained evidence |
| --- | --- | --- |
| C01 | Application storage, startup, logging, About and language selection | [CP32, CP59, CP67, CP68](docs/linux-desktop-settings.md) |
| C02 | USB/SCSI/MMC discovery, HDD/card filtering and protected target identity | [CP17, CP65, CP71](docs/linux-device-discovery.md) |
| C03 | Image entry, analysis views, source identity and supplementary ZIP | [CP44, CP57, CP69](docs/linux-iso-options.md) |
| C04 | MD5/SHA1/SHA256 and optional SHA512 hashing | [CP23](docs/linux-feature-parity.md) |
| C05 | MBR/GPT/SFD layouts, old BIOS fixes and ISO-to-ESP | [CP16, CP51, CP52](docs/linux-feature-parity.md) |
| C06 | FAT16/FAT32/NTFS/exFAT/ext2/ext3 and UDF through 2.01; quick/full, labels, NTFS options | [CP47, CP60, CP65, CP70](docs/linux-filesystem-formatting.md) |
| C07 | DD/compressed/container deployment, relative VHD/VHDX parents/log replay and FFU feature set | [CP24-34, CP38-41, CP43, CP46, CP50, CP53-56](docs/linux-ffu-parity.md) |
| C08 | Device VHD/VHDX/FFU saving, UDF filesystem ISO export and read-only discovery/unmount | [CP35-37, CP46, CP56](docs/linux-support.md) |
| C09 | Windows To Go and standalone WIM/ESD native deployment | [CP58](docs/linux-windows-to-go.md) |
| C10 | Qualified Windows 7/8/8.1/10/11 and WinPE creation; applicable customization flags | [CP11n, CP22, CP58, CP61](docs/linux-windows-installer.md) |
| C11 | Qualified ISO/standalone GRUB2, GRUB4DOS, Syslinux, FreeDOS, MS-DOS and ReactOS choices | [CP18-22, CP64](docs/linux-legacy-boot.md) |
| C12 | Image/payload downloads, policy refresh and native application release checking | [CP21, CP63, CP66, CP68](docs/linux-application-updates.md) |
| C13 | Persistence, runtime UEFI validators and bundled/remote Secure Boot policy | [CP42, CP61-63](docs/linux-runtime-validation.md) |
| C14 | Full/fast zeroing, bad-block patterns, fake-capacity detection and recovery choices | [CP45, CP48](docs/linux-support.md) |
| C15 | Dialogs, control lifecycle, one authorization, cancellation and sync/cleanup/exit before READY | [CP12-17, CP32-71](docs/linux-gui-parity.md) |

FFU feature implementation is closed by CP56. Do not reopen it for inferred
multi-target behavior: the Windows desktop writes a selected disk/store.
Windows To Go metadata already includes ACLs, ADS, hardlinks, reparse points,
timestamps, EAs and encrypted EFS content; decryption and installed-Windows
execution are not newly inferred implementation gaps.

## Explicit differences and false backlog items

These dispositions preserve prior user decisions and prevent dormant code or
platform housekeeping from becoming unnecessary feature projects.

- **E01 — Dormant ext4 format enum.** FS_EXT4 has no enabled Windows UI cluster entry; format_ext.c falls back to ext3. Native ext4 persistence already exists. This is not a missing selectable ext4 data formatter. References: [src/rufus.c:476](../src/rufus.c#L476), [src/format_ext.c:275](../src/format_ext.c#L275).

- **E02 — Unused AdvancedMode key.** SETTING_ADVANCED_MODE has no exact runtime reader; ShowAdvancedDriveProperties and ShowAdvancedFormatOptions are the active, implemented panel settings. References: [src/settings.h:31](../src/settings.h#L31), [linux/qt/main_window.cpp:297](../linux/qt/main_window.cpp#L297).

- **E03 — Dead DisableSecureBootNotice setting.** Only generic notification code writes it; there is no active reader in this baseline. Do not invent an extra feature from the macro. References: [src/stdlg.c:726](../src/stdlg.c#L726).

- **E04 — Developer-only test/version helpers.** Ctrl+T is compiled only for debug/TEST/ALPHA; IDC_TEST is hidden test machinery; -z forces Windows-version detection. These are not offered release media features. References: [src/rufus.c:3745](../src/rufus.c#L3745), [src/rufus.c:3164](../src/rufus.c#L3164).

- **E05 — Session runtime-validation preference.** Windows reading/writing EnableRuntimeValidation is commented out; native session-only behavior is correct. References: [src/rufus.c:3494](../src/rufus.c#L3494).

- **E06 — Windows autorun registry cleanup.** Alt+X restores Windows NoDriveTypeAutorun state; native code never creates that registry/policy change. No Linux device action is required. References: [src/rufus.c:3988](../src/rufus.c#L3988).

- **E07 — Agreed host regional-settings behavior.** Copy current Windows regional settings remains visible and disabled on Linux. Explicit source/user choices replace no other customization option; further host-setting inference is prohibited. References: [AGENTS.md:115](../AGENTS.md#L115).

- **E08 — Native package update ownership.** CP68 documents why native multi-file packages update through distribution/package workflows rather than a Windows executable replacement. Actual release delivery remains Q03/Q04. References: [docs/linux-application-updates.md:13](../docs/linux-application-updates.md#L13).

- **E09 — Agreed FAT32 caption.** Linux uses FAT32 at all sizes. This settles the name only; Alt+L/V observable formatting effects remain A02. References: [docs/linux-gui-parity.md:42](../docs/linux-gui-parity.md#L42).

- **E10 — Explicit DOS and initial language choices.** FreeDOS starts at US/437 and language at English with explicit saved/menu choices, per user instructions. Do not add host locale/keyboard detection for parity. References: [AGENTS.md:72](../AGENTS.md#L72), [docs/linux-localization.md:10](../docs/linux-localization.md#L10).

- **E11 — Autorun attribute claim corrected.** Windows icon.c creates autorun.inf/icon as ordinary files, not hidden/system files. Kolibri-specific hidden/system loader attributes remain I19. References: [src/icon.c:161](../src/icon.c#L161).

- **E12 — Shared label normalization.** format_policy.c NormalizeVolumeLabel is shared, including FAT non-ASCII replacement. Do not invent a Linux-only OEM-label gap. References: [src/format_policy.c:285](../src/format_policy.c#L285).

- **E13 — Native update consent bookkeeping.** Windows CommCheck64/internal network state is represented by explicit native application/policy consent and separate timestamps. It is not another missing user setting. References: [docs/linux-application-updates.md:57](../docs/linux-application-updates.md#L57).

- **E14 — Windows autoplay group policy.** DisableLGP controls Windows local group-policy mutations; native private mounts do not make those changes. Documented one-helper cleanup remains C15. References: [src/stdfn.c:1002](../src/stdfn.c#L1002), [linux/platform/target_mounts.cpp:309](../linux/platform/target_mounts.cpp#L309).

- **E15 — Case-insensitive imported setting names.** ApplicationStorage key lookup is case-insensitive and canonicalizes existing aliases; EnableVmdkDetection/EnableVMDKDetection and DisableRufusMBR capitalization are not missing imports. References: [linux/qt/application_storage.cpp:27](../linux/qt/application_storage.cpp#L27).

- **E16 — IA64/EBC runtime validators.** Windows has six bundled validators, not eight. Native resources preserve IA64/EBC entries without a validator, matching the original resource set; EFI creation remains I16. References: [linux/platform/media_integrity.cpp:478](../linux/platform/media_integrity.cpp#L478).

- **E17 — Deferred original Windows execution.** The user deferred original Windows execution and further installed-OS/customization VM tests. CP11n fulfills the representative customization creation/readback scope. No new matrix is authorized. References: [AGENTS.md:126](../AGENTS.md#L126).

- **E18 — FFU multi-target requirement corrected.** CP56 proves Windows desktop uses one selected disk/store. Automatic multi-target/UEFI-LUN mapping is not an offered feature and does not reopen FFU. References: [docs/linux-ffu-parity.md:23](../docs/linux-ffu-parity.md#L23).

## Coverage and audit limits

Every entry below maps to delivered work, a specific open item or an explicit
disposition in the JSON register. Resource counts include captions and other
informational IDs; they are not a count of independent features.

| Inventory | Entries |
| --- | ---: |
| shortcuts | 40 |
| settings | 33 |
| dynamic settings | 4 |
| resource controls | 132 |
| dispatch commands | 27 |
| dynamic toolbar | 5 |
| startup | 10 |
| boot choices | 10 |
| filesystems | 9 |
| efi architectures | 8 |
| host architectures | 3 |
| unattend flags | 13 |
| media operations | 14 |

The filesystem inventory includes dormant ext4, explicitly classified E01;
there are eight selectable formatting filesystems in the Windows baseline.
Shortcut entries include developer-only Ctrl+T and grouped plus/minus aliases.
The dialog inventory covers all ten resource dialogs, plus dynamic toolbar and
menu dispatch separately. Startup coverage includes documented long/short
options, early /W and /F aliases, portable INI mode and the developer -z switch.
Dynamic settings include the eight VID/PID slots, eight disk GUID slots and ESP
restoration records. Boot/source classes and all named customization flags are
mapped separately from the visible controls.

A disposition audit is not a claim that every hardware, firmware or provider
case has run. Existing checkpoint receipts remain the evidence for delivered
behavior. CP71's last nine-suite Qt gate recorded 2,206 passes, five old optional
skips and zero failures; this audit did not rerun it. Original Windows execution
and additional installed-Windows/customization tests remain deferred (E17).

Audit artifacts: [source hashes](../../.rufus-test-artifacts/parity-audit-2026-10-23/baseline.json),
[validation receipt](../../.rufus-test-artifacts/parity-audit-2026-10-23/validation.json)
and [resume record](../../.rufus-test-artifacts/parity-audit-2026-10-23/RESUME.md).
Structural validation checks source anchors, referenced files, unique work
assignment, dependencies, source-derived control/settings/dispatch coverage and
all 394 baseline source hashes. It makes no new runtime claims.

## Finish criteria and scope changes

Full feature completion requires all I/A rows to close through implemented and
qualified behavior, or a demonstrated native difference meeting AGENTS.md and
existing user decisions. Release signoff additionally closes Q01–Q04 with
supported-boundary disclosures; unavailable physical hardware or publication
access must remain visibly unqualified rather than marked as passing.
Debian/RPM packaging in Q03 is a bounded Linux delivery goal, not an additional
Windows media feature. Deferred Windows execution is disclosed, not converted
into an installation test gate.

Any newly found item needs a concrete offered Windows dispatch/eligibility path,
native counter-evidence and de-duplication against this register. Assign it to
the owning batch and report the scope change. Broad “other versions,” “more
combinations,” hypothetical device models and dormant enum values cannot create
new checkpoints. A defect found in a closed path is fixed in its owning batch;
unchanged areas retain their prior evidence.
