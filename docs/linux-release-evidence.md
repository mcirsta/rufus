# Linux release evidence reconciliation (CP78)

This is the Q01 coverage index for preparing a useful release. It reuses the
retained qualification behind delivered features; it does not declare complete
Windows parity or qualify a physical USB by treating a simulated device as one.
The machine-readable index is
[`release-evidence.json`](../linux/packaging/release-evidence.json). The
[parity register](linux-parity-register.json) owns the current release status.

## Reuse boundary

The CP78 audit compared all 197 production C/C++/header files in `linux/helper`
and `linux/platform` against the CP77 delivered source manifest, the CP78 starting
snapshot and the current tree. They match. `cp78/media-source-reuse.json` records
the individual hashes. GUI, localization and packaging changes require their
candidate checks; unchanged media algorithms do not need another family-by-family
write/boot matrix. Later functional media changes invalidate the affected reuse
claim and need a focused case.

The JSON index records hashes for 26 retained checkpoint receipts, the document
and checkpoint chain for each boot choice, and all 40 original shortcut entries,
33 settings, four dynamic-setting groups and 13 unattended flags with their
owning C/I/A/E rows. This includes advanced/expert controls and explicit original
Windows differences. Technical logs and an unused enum do not invent extra
user-facing features. The index does not multiply policy-ineligible combinations.

Receipts live in the development workspace's `.rufus-test-artifacts` directory;
installed documentation does not claim those logs are bundled. Earlier receipts
retain their original source and qualification boundaries. Later shared regression
and feature-batch gates carry forward that evidence; source identity alone is
not a replacement for a runtime result.

## Source archive completeness

The original `.gitattributes` excludes shell scripts from Git tag archives.
The narrow `linux/**/*.sh -export-ignore` override retains the six native
build/qualification shell scripts while preserving the original Windows and
root maintenance-script exclusions. Required wimlib patches and signed download
fixtures also have narrow `.gitignore` exceptions; a plain file listing alone
would not establish Git archive behavior.

The CP78 audit created a private index/object store from the current candidate
and ran `git archive` using its actual tree attributes. All 914 required native
inputs were exported, including 282 configured GUI/helper/native-provider source
inputs, 43 QRC resource inputs and all six native shell scripts. Export bytes
match Git's eol conversion rules, and `res/loc/rufus.loc` remains byte-identical.
The real Git index was unchanged and temporary objects were removed. Evidence:
`cp78/tag-archive-input-audit.json`. Separate pinned wimlib/Syslinux upstream
archives are declared and verified AUR sources, not missing Rufus members.

This verifies local Git export semantics. After the authorized `linux-preview.47`
publication, inspect the actual GitHub archive and use its own SHA-256 in final
AUR metadata. A locally generated compressed archive does not establish the
public archive's checksum.

## Ten boot choices

| Choice | Retained creation/readback and firmware evidence |
| --- | --- |
| Non bootable | CP76: seven filesystems at 512/512, 512/4096 and 4096/4096, rotating MBR/GPT/SFD; CP77: supported-UDF metadata policy. No bootloader is installed. |
| MS-DOS | CP64: FAT16/FAT32, original records/resource and explicit keyboard/code-page bytes; CP69: original explicit large-cluster override. No Microsoft application is executed. |
| FreeDOS | CP19d2b/c and CP19e: helper/readback, ordinary Qt and real FreeDOS BIOS boots on FAT16/FAT32; CP51: eligible old-BIOS geometry. |
| Disk or ISO image | CP56: completed FFU feature audit; CP58: To Go/WIM/ESD device jobs; CP73: parent/source operations; CP74/75: ISO/EFI/legacy source routes. The support guide names each input and its precise boundary. |
| Syslinux 4.07 | CP20e–h: standalone FAT16/FAT32 Qt/helper/readback and original bare BIOS loader; CP74: source-matched providers and aarch64 accessor portability. |
| Syslinux 6.04 | CP20a–d/i–m: standalone FAT/NTFS Qt/helper/readback and bare BIOS loader; CP74: source-matched resources. |
| ReactOS | CP64: standalone FAT16/FAT32 original records and native marker loaded through the real PBR; CP75 separately qualifies the ReactOS ISO route. No ReactOS application runs. |
| Grub4DOS | CP22n47/49/53: standalone FAT32/FAT16/NTFS; CP22n71 and its prior chain: ordinary ISO creation/readback and original BIOS/UEFI loader handoffs. |
| Grub 2.14 | CP21b–l: standalone FAT16/FAT32/NTFS/ext2/ext3, ordinary creation/readback and original bare BIOS loader; the GRUB2 guide records later ISO routes. |
| UEFI:NTFS | CP18a–e: standalone GPT/MBR NTFS/exFAT and Qt; CP74 and retained ISO/To Go gates: original embedded UEFI:NTFS handoff. A blank data filesystem is not an OS. |

See [creation modes](linux-support.md#creation-modes),
[standalone legacy boot](linux-legacy-boot.md),
[GRUB2](linux-grub2-reuse.md), [GRUB4DOS](linux-grub4dos-reuse.md),
[ISO/EFI resources](linux-iso-efi-resources.md) and
[legacy ISO routes](linux-legacy-iso-routes.md).

## Filesystems, layouts and geometry

Seven native data filesystems remain active: FAT16, FAT32, NTFS, exFAT, UDF through
2.01, ext2 and ext3. The original eight selectable Windows filesystems include
ReFS, which the user deferred. The original ext4 data-format enum is dormant
(E01); ext4 persistence is a separate supported operation.

CP76 independently reads back all seven native filesystems with real logical and
physical sector facts at 512/512, 512/4096 and 4096/4096. Each provider's capacity,
cluster and block bounds remain enforced before writes. MBR, GPT and SFD are
qualified where the original policy and provider admit them. See the
[geometry contract](linux-sector-geometry.md).

Data formatting and native FAT UEFI support 4Kn under emulation. Original
BIOS/DOS/Syslinux/SBR and bundled UEFI:NTFS retain their 512-byte executable
contract and reject unsupported logical sectors before writes. NTFS on 4Kn
preselects NTFS-3G for file creation before partitioning; it does not change
providers after an I/O or corruption error. Physical 4Kn hardware remains a
separate Q02 qualification.

CP77 retains supported UDF formatting and the saved/frozen Alt+Q choice. When the
optional Windows indexing attribute cannot be written, a clear nonfatal diagnostic
is emitted and the job still completes normal sync/cleanup/helper exit. **The
Windows UDF indexing attribute is not written on disk.** That is a qualified
native release limitation (A03), not completed metadata parity.

## Current candidate checks

The grouped normal and sanitizer component runs each pass 105 localization and
67 application-update Qt entries, with no failures or skips. Excluding the four
suite lifecycle hooks gives 168 substantive cases per build; the same normal
and sanitizer cases are counted once. The opt-in actual published-release check
is a separate later qualification.

The source inventory records 557 distinct literal Qt sources at 677 occurrences.
At the English catalog, 193 are catalog-bound and 364 have no original binding.
All 38 languages are checked, with original inheritance and invalid-placeholder
refusals recorded separately, including two Arabic source fallbacks. This is
source evidence with documented English fallback; it does not claim every
branch is reachable or that native-only text has full translation coverage.

Debian 13 and Fedora 44 rootless-container qualification each passes a clean
native source build, fresh install, upgrade, removal, user-state preservation,
unprivileged offscreen GUI/help/version/single-instance and installed path/library
checks. The upgrade fixture uses the same candidate payload with a lower package
release, not an older published Debian/Fedora release. Desktop authorization,
kernel filesystem drivers and physical media are not qualified by containers.
Optional cdrtools `mkisofs` was not supplied/qualified in those environments.
See `cp78/distributions/qualification.json`.

One corrected local Arch/AUR candidate source build completed in 73.94 seconds
and its runtime inspection passed. Final direct runtime dependency metadata fixes
do not change the compiled product. Final public-source re-render/verifysource remains pending until the tag is
public; no second product compile is required when the compiled inputs match.
The release owner will set the maintainer identity and submit to AUR manually. The earlier incomplete-source and fixture failures
remain in the receipts. See `cp78/aur/qualification.json`. The source-stability receipt also confirms
all 929 recorded production inputs (`src/**` and non-test/non-packaging Linux
sources) match the corrected source candidate consumed by that clean build.
Subsequent docs/package metadata/export-rule and test-only changes preserve that
compiled product; see `cp78/production-source-stability.json`.

The pinned Windows/instruction check compares only `src/**` and `AGENTS.md`:
all 125 files match the baseline. The baseline contains 394 entries; this is
**not** a claim that all 394 entries, including changed Linux code/docs, match.
See `cp78/pinned-windows-baseline.json`.

The single full nine-suite candidate Qt gate passes in 465.47 seconds:
2,217 substantive cases, zero failures and two existing optional real-source
selection skips. Product code is unchanged after the gate.

The bounded desktop gate passes 32 substantive cases, zero outstanding failures
and no skips on KDE native Qt Wayland and Qt xcb through actual XWayland, each at
1x and 2x scale. It covers live French/Arabic/Romanian relabel/RTL, keyboard device
choice/tab chain, dialog close/reopen, main Escape and 18-point font layout.
Twelve application-only screenshots were retained and representative normal/2x
views were reviewed. Two test-only methods were repaired to wait for native
activation/modal focus restoration and Escape closure; their normal test target
was rebuilt after the full gate. The 929 production inputs remain unchanged.
See `cp78/desktop-qualification.json`.

Qt xcb under XWayland is the qualified X11 path. A standalone Xorg session remains
unqualified: the host's `kwin_x11` has a ScreenLocker ABI mismatch. No host packages
were repaired. Keyboard events are Qt test events rather than physical input
hardware; physical media and additional architecture hosts retain their separate
Q02 gaps. Initial desktop attempts and focused repairs remain in the receipts;
the full Qt gate was not repeated.

## Candidate results and remaining qualification

| Owner | Current boundary and next evidence |
| --- | --- |
| I05 release policy | The user accepted original Rufus catalog reuse and documented English fallback for Linux-specific text. Full native translations remain follow-up parity work and do not block the first release. The catalog/template inventory and candidate checks qualify this bounded release scope. |
| Q01 | This index reconciles retained choices. CP58 proves x64 To Go device jobs and an EFI marker; ARM64 CA2023 boot-tree bytes have component coverage only. No retained completed ia32/ARM32 To Go device/firmware route was identified. A bounded non-x64 creation/readback and applicable EFI handoff remains visible; do not add installed-Windows tests. |
| Q02 | The bounded native Wayland and Qt xcb/XWayland 1x/2x desktop checks pass as recorded above. Standalone Xorg and complete i686/aarch64 application host build/run remain unqualified; CP74's isolated Syslinux cross-build does not establish them. No usable writable physical USB or native 4Kn disk is available: physical write/readback, unplug-after-READY and reconnect remain unqualified. |
| Q03 | Debian 13 and Fedora 44 clean native builds, fresh install, same-payload lower-release upgrade and removal passed. Dependencies, libraries and helper/polkit/resource/provider paths were checked. The corrected local AUR source candidate also built cleanly and passed runtime smoke; its final public-source re-render/verifysource remains Q04. |
| Q04 source-candidate phase | The user authorized GitHub commit/push/tag publication of `linux-preview.47` at `mcirsta/rufus` as the AUR source. Freeze prepublication proof, then record the actual public source commit/tag/archive hashes and live publisher/update handoff in the final attached delivery receipt. This source document does not claim those post-tag checks have run; local fixtures and CP68's dated empty catalog do not qualify them. |

`cp78/host-capabilities.json` records the read-only hardware/tool inventory. No
physical host media were opened, mounted, formatted or reset by this audit.
The presence of a USB transport with zero capacity or read-only media does not
supply a writable test target. Unavailable hardware is disclosed; it does not
prevent preparing reviewable local release assets.

## Explicit release deferrals

The user's deferrals remain gaps in eventual full parity:

- I14/B6: UDF 2.50/2.60 metadata-partition formatting. Existing UDF 1.02–2.01 stays.
- I15: ReFS formatting.
- I10: actual Microsoft Dev Drive membership detection. There is no blanket ReFS
  virtual-disk refusal or label-based replacement.

Original Windows execution and installed customization effects remain unverified.
CP11n fulfills the agreed representative real-ISO creation/readback scope with
only “Skip privacy questions” selected. No additional OS-installation matrix is
required or implied here.

## Bounded candidate gate

Use focused localization/control checks during changes, then one full nine-suite
Qt gate for the final candidate. Reuse unchanged media/helper/fault/boot evidence.
Run the bounded desktop and package checks above in groups, fixing concrete
failures in their affected group. Record matching source/package hashes and
remaining unavailable qualification before the authorized GitHub publication. Verify
the actual public tag/archive afterward; a local archive checksum is not assumed
to match GitHub's compressed bytes. The final attached delivery receipt records
these postpublication checks. Keep this source-candidate record frozen instead
of retagging it to embed its own public archive receipt.
