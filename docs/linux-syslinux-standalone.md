# Standalone Syslinux reuse boundary — CP20a

Audited 2026-09-29 against this Rufus checkout and the already verified local
syslinux-6.04-pre1 release. **Reuse the existing native installer, partitioning,
formatting, process runner and readback.** The missing FAT feature is a
source-free entry into that path, not another Syslinux implementation.

## Windows behavior and native coverage

| Area | Original Windows behavior | Linux reuse / remaining work |
| --- | --- | --- |
| Boot choice | `src/rufus.c:SetBootOptions` lists embedded Syslinux 4.07 and 6.04 under advanced drive properties. Collapsing the section restores image mode. | Reuse the existing advanced-selector behavior after the backend is qualified. Selecting a standalone version is explicit; it must not manufacture an ISO report or infer an unknown image version. |
| Layout and filesystem | `SetPartitionSchemeAndTargetSystem` restricts both to MBR/BIOS. `src/format_policy.c:GetAllowedFileSystems` permits FAT16/FAT32 for v4, plus NTFS for v6. | Share those rules. The qualified native mtools installer covers FAT16/FAT32/512-byte sectors. NTFS for v6 remains required parity work, not an unsupported Windows combination. Other geometries/BIOS fixes remain separate. |
| MBR | `src/format.c:WriteMBR` uses the original Syslinux bootstrap and an active partition. | Existing MBR writer already uses that payload, preserves the disk signature and validates FAT16/FAT32 partition placement. FreeDOS's default Rufus MBR must not carry into this mode. |
| Loader/VBR | `src/syslinux.c:InstallSyslinux` writes the selected embedded loader plus ADV, maps its sectors through libfat, patches it, then writes the VBR. | The pinned native installer embeds the same v6 loader and VBR. Existing `syslinux_install.cpp` checks geometry/identity, runs it over the retained descriptor, then verifies the patched loader/ADV, VBR, FAT chains and companion. Reuse that whole operation. |
| v6 companion | Windows's boot check reuses or offers to download `ldlinux.c32` matching the embedded v6 version. Declining can proceed without it; copy failure is also logged rather than fatal. | The matching 122656-byte companion is already embedded in the native executable and hash-pinned by CMake. There is no missing payload to download. Require successful installation/readback; do not report READY with a known missing boot dependency. This follows the port's existing complete-media rule. |
| Output | v4 needs `ldlinux.sys`; v6 adds `ldlinux.c32`. This route creates no `syslinux.cfg`, kernel, menu or OS. ReactOS's separate branch creates its own configuration. | Preserve the bare loader result. Do not generate a sample Linux configuration or pretend this installs an OS. CP20a verifies the native v6 output and bare BIOS prompt. Optional extended label/icon files are separate existing backlog work. |
| Progress | `PrintInfoDebug(MSG_234)` says `Installing Syslinux %s...`. Shared progress reserves three copy slots, but `InstallSyslinux` emits **two** negative `OP_FILE_COPY` updates, after the loader write and at successful completion. | Preserve the allocation and actual two-event behavior; do not demand three events just because three slots are reserved. Native CLI output supplies no intermediate milestones, so report completed steps after successful installation/readback rather than inventing timed progress. Finalization completes the remaining range, as on Windows. |
| Completion | Formatting/remount/finalization precede success. | Keep one helper for the entire job and existing cancellation/process-group cleanup, filesystem/device sync and exit-before-READY. Standalone FAT installation needs no private filesystem mount: mtools writes through the same retained disk descriptor used by the ISO route after unmount. |

The native provider pads the loader to a sector before appending two ADV sectors;
its installed v6 file is 69632 bytes. Windows writes the unpadded embedded length
before ADV. Our existing readback already accounts for native padding and checks
original Rufus patching against the actual FAT chain. Raw final file sizes need
not be identical for the same boot behavior. The native installer also marks
both boot files read-only/hidden/system; Windows explicitly sets those attributes
on `ldlinux.sys` and copies `ldlinux.c32` from its cache. Keep that existing native
protection of boot components explicit; do not claim byte/attribute identity
with Windows's cache-dependent result. No provider changes were made in CP20a.

## Payload identities and version boundary

`res/syslinux/ldlinux_v4.sys` identifies itself as **4.07 (2013-07-25)** and is
36488 bytes; its VBR is 512 bytes. The v6 resource is **6.04-pre1**, 68121 bytes,
with a 512-byte VBR. The v6 pair matches the verified upstream release exactly;
`linux/syslinux/CMakeLists.txt` already enforces that equality and pins the
companion hash. The audit's complete hashes/banners are in parent
`.rufus-test-artifacts/cp20a-payload-manifest.json`.

The current native executable is a fixed v6 build with compiled-in payloads.
It cannot switch to the v4 pair through a runtime option. Do not label that
executable as a 4.07 provider or substitute v6 for the user-selected v4.
A later checkpoint must verify an upstream 4.07 release, match its payloads to
Rufus, and select/build its native installer through CMake. CP20e later verifies/builds the official 4.07 release below; it does not yet
connect that provider to the application. Keep the original license notices and
corresponding upstream source when distributing either provider.

For NTFS, the inspected release's `extlinux/main.c` already has an NTFS install
path, unlike the current mtools executable, which always finishes with a FAT VBR.
It is a candidate to investigate before writing an NTFS installer ourselves.
It depends on a mounted directory, native file-sector mapping, device discovery
and filesystem identification; its FUSE recognition alone does not establish
safe NTFS-3G behavior. Qualification must cover the preferred kernel NTFS driver
and the fallback, exclusive claim/mount lifecycle, exact boot readback and one
authorization. No NTFS installer support is claimed here.

## Executable probe and evidence

`linux/probes/syslinux/test_standalone.py` creates two disposable 64-MiB regular
files. It uses the existing MBR and native formatter probes, then calls the
checked adapter, which invokes the unchanged pinned native installer on an empty
FAT16 or FAT32 partition. CP20a established the direct-installer baseline;
CP20b moves this probe onto the production adapter. No fake
ISO, copied configuration, sample kernel, root authorization or host mount is
involved. The new `rufus_syslinux_standalone_probe` CTest entry repeats this check.

Both cases pass: exact two-file inventory, companion byte equality, native loader
size/attributes, independent FAT check, preserved BPB and FAT32 backup VBR,
original MBR and unchanged bytes outside the partition. Each unchanged disk then
boots as a virtual USB through SeaBIOS to the Syslinux banner, the expected
`WARNING: No configuration file found` and `boot:` prompt. QEMU snapshot mode
leaves backing-image SHA-256 unchanged. This proves a bare-loader boot, not an
OS handoff, a production standalone helper job or every physical firmware.

The probe reuses existing FAT readback, MBR checks, firmware recording and QMP
screen capture. Its filesystem check normalizes the known primary/backup VBR
difference on a throwaway copy only, as existing Syslinux tests do. Compact
screenshots, prompt text, hashes and logs remain under parent
`.rufus-test-artifacts/cp20a-standalone-verified`; disks and temporary trees are
removed. No Windows customization test is involved.

## Next small checkpoints

1. **CP20b completed:** `NativeSyslinuxRequest` / `InstallNativeSyslinux` accept
   an explicit `BT_SYSLINUX_V6` selection with all four ISO/source fields null.
   `BT_IMAGE` remains the default and requires all four fields. Wrong versions,
   invalid/mixed selections and unsupported geometry fail before the installer
   starts. Standalone requires the freshly formatted root's end marker, optionally
   preceded by its volume label; existing/deleted files, directories and long-name
   records are rejected. This is a creation step, not an existing-volume updater.
   Common geometry and companion identity are separated from image eligibility;
   target checks, process supervision, payload reconstruction/readback, sync and
   cleanup remain shared. The existing helper and VM caller only adopt the new
   API names; neither gains a standalone request in this checkpoint.
   Both FAT variants pass 46 standalone and 43 image adapter cases, normal and
   sanitized. The file-only MBR probes also boot through the checked adapter;
   compact evidence is in parent `.rufus-test-artifacts/cp20b-standalone-verified`.
2. **CP20c completed:** the bounded helper operations
   `prepare-syslinux6-fat16-device-v1` and `prepare-syslinux6-fat32-device-v1`
   select the pinned 6.04-pre1 provider explicitly. Existing image operations
   remain source-backed; mixed requests and other versions/filesystems are
   rejected. One helper handles acquisition, original Syslinux MBR, native FAT
   formatting, installation/readback and final disk sync. No extraction/mount is
   needed. The protocol accepts exactly two copy steps during bootloader and
   requires finalizing plus helper exit before success. Copy byte counters stay
   zero because this route does not extract image files.
   Both FAT variants pass eleven native guest cases, independent readback and
   bare BIOS boots. A guest-only launcher proves one launch and delayed-exit
   behavior; no new real polkit prompt or physical firmware test is claimed.
   Seven normal and seven sanitizer suites cover requests/processes and existing
   helper modes. Evidence is in parent `.rufus-test-artifacts/cp20c-*`.
3. **CP20d completed:** the advanced Boot selection now offers **Syslinux 6.04**,
   matching Windows's displayed major/minor version (the pinned payload is still
   6.04-pre1). It offers MBR, BIOS (or UEFI-CSM) and qualified FAT16/FAT32 choices.
   Shared Rufus filesystem/cluster policies retain valid preferences and defaults.
   Hiding advanced properties restores the retained image; selecting an image
   works from this mode. No retained ISO or Windows/DOS customization enters the
   standalone request. GUI preflight checks formatting, geometry and native tools
   before confirmation and again before the single authorization.
   The existing controller uses the shared three-slot/two-event progress, shows
   `Installing Syslinux 6.04...`, and requires sync/cleanup/helper exit before full
   READY and one completion notification. There is no added service or installer.
   Normal/sanitized GUI cases and screenshots are in parent
   `.rufus-test-artifacts/cp20d-*`. The native qualification remains CP20c's; the
   new GUI tests use a protocol fixture and never write physical media.
4. **CP20e completed:** the optional `rufus-syslinux4` target shares the CMake
   recipe, compiles nine unmodified 4.07 source files and embeds the two original
   release payloads. They exactly match Rufus's v4 resources; hashes are pinned
   independently. A v4-only build and a combined v4/v6 build both work, and the
   v6 executable hash is unchanged. Staging uses `Syslinux4Runtime` with ordinary
   permissions and the original license/source notice.
   The existing regular-file runner passes supervision/cancellation/final-sync
   checks with this provider. FAT16/FAT32 native/staged media contain only the
   padded loader plus ADV (37888 bytes), preserve BPB/backup/gaps, and boot as
   snapshot virtual USBs to the bare 4.07 prompt. Its `ERROR: No configuration
   file found` is expected without a supplied OS. 4096-byte sectors are refused
   without writes, and altered payloads fail configuration. Compact evidence is
   in parent `.rufus-test-artifacts/cp20e-*`; disposable media/build/staging trees
   are removed. This is x86-64 provider/runner qualification, not checked adapter,
   native-device/helper/GUI qualification or a sanitizer-clean upstream claim.
5. **CP20f completed:** the same checked adapter accepts explicit standalone
   v4 with null source fields, selecting its pinned Rufus payload pair and padded
   length. The existing FAT reader, original patcher, runner, sync and cleanup
   verify the complete loader/ADV/VBR; v4 needs no companion. Both payloads have
   enough extent slots even for a completely fragmented loader. Wrong-version
   providers fail readback after writes. Image compatibility stays unchanged.
   All 274 adapter cases pass normally and under ASan/UBSan/leak checking using
   normal upstream executables. Build-tree/staged FAT16/FAT32 bare BIOS boots
   now use the checked v4 adapter. Package, selection and helper-refusal
   regressions pass; optional-provider configurations also pass. Evidence is
   in parent `.rufus-test-artifacts/cp20f-*`; disposable media are removed.
6. **CP20g completed:** explicit v4 FAT16/FAT32 helper operations use the same
   native job and fixed provider mapping as v6/image jobs. Original MBR, format,
   checked installation, final sync and cleanup remain shared. Both versions
   retain two copy events and wait-for-exit completion through one authorization.
   Seven normal/sanitized suites pass, including 48 shared request-test results.
   Forty-four standalone native guest jobs cover both versions/FATs; all four
   completed disks pass independent readback and bare BIOS boot. Five existing
   ISO helper jobs and configured boot handoff also pass. Evidence is in parent
   `.rufus-test-artifacts/cp20g-*`; disposable media/guest trees are removed.
7. **CP20h completed:** the advanced Qt chooser lists v4 before v6, with the
   Windows labels, MBR/BIOS/FAT choices and default-cluster reset when changing
   boot selection. Collapsing advanced properties restores the retained image.
   Both versions use the existing controller, selected-provider preflight,
   versioned status, shared progress and exit-before-READY gate. An unavailable
   other-version installer does not block a job. Normal/sanitized GUI tests and
   v4/v6 screenshots are in parent `.rufus-test-artifacts/cp20h-*`; no native
   installer/helper change or additional VM qualification was needed.
8. **CP20i completed:** [the native extlinux audit/probe](linux-syslinux-ntfs.md)
   installs v6 on NTFS through both the preferred kernel driver and NTFS-3G.
   Native file mapping, raw sector comparison, mount claims, independent readback,
   kernel remount and bare BIOS boots pass. ntfsprogs-plus's restricted jump
   checker rejects the original Syslinux VBR; record that incompatibility and
   keep Rufus's original bytes. No production route is enabled by this probe.
9. **Next: CP20j.** Wrap the NTFS provider in checked installation/readback and
   failure/cancellation tests before helper/GUI exposure. Reuse the original
   patcher, existing native mount lifecycle and command runner. CMake still
   refuses broken upstream v4 non-x86 branches until corrected; standalone v4
   does not enable v4 ISO jobs.
