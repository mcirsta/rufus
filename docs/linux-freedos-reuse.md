# FreeDOS reuse boundary — CP19a–e

Audited 2026-09-28 against the current Rufus source, after CP18e. **Reuse the
bundled files and the existing FAT boot-record routines.** Add a bounded native
I/O adapter, then connect them to the existing formatter and authorized job.
Split DOS locale selection/rendering from Windows host queries rather than
porting the whole Windows extraction module or duplicating its tables.

CP19a adds a test-only native compilation probe. CP19b qualifies the native
boot-record adapter on formatted regular files. CP19d completes native-helper
creation/readback and representative BIOS boots; CP19e enables the normal Qt
FreeDOS choice. Remaining advanced BIOS/geometry options are tracked separately.
CP19c1 extracts shared DOS locale policy/content and connects the existing
Windows wrapper to it. CP19c2 shares the payload manifest and qualifies native
copying/FAT attributes. CP19c3a publishes startup files through the shared writers
and the same native copy I/O. CP19c3b provides an explicit Qt keyboard/code-page
chooser instead of host detection (user decision 2026-09-29). Complete
helper/boot qualification remains before exposing FreeDOS creation.

## Original behavior and reuse

| Operation | Windows source and behavior | Linux reuse boundary |
| --- | --- | --- |
| Boot choice | `src/rufus.c:SetBootOptions` includes FreeDOS even with advanced drive properties collapsed. | Add BT_FREEDOS to the existing selector once its backend is qualified. It is not an advanced-only choice like UEFI:NTFS. |
| Layout and filesystem | `SetPartitionSchemeAndTargetSystem` offers MBR and BIOS (or UEFI-CSM); `GetAllowedFileSystems` permits FAT16/FAT32. No SFD or GPT for this choice. | Reuse shared filesystem/cluster/partition rules and native geometry checks. Default filesystem markers are assigned before boot filtering; preserve the existing/manual filesystem preference where valid. |
| MBR | `src/format.c:WriteMBR` sets an active FAT16 LBA (`0x0e`) or FAT32 LBA (`0x0c`) partition. FreeDOS reaches the conventional Rufus/Windows fallback. `use_rufus_mbr` defaults true; its Alt+A/DisableRufusMBR setting selects Windows 7 code instead. | Extend the existing libfdisk MBR writer's bootstrap choice. Reuse `mbr_rufus.h` and `mbr_win7.h` directly, as the current writer reuses the Syslinux header. Do not silently use Syslinux's MBR. Preserve the table and disk signature; both boot payloads are 440 bytes. |
| FAT formatting | Windows creates the filesystem before replacing its boot record. | Keep native dosfstools and the existing descriptor handoff, selected geometry, BPB verification and sync. It already supplies BIOS drive `0x80`, fixed-media byte `0xf8`, hidden sectors and a FAT32 backup at sector 6. No FreeDOS formatter is needed. |
| Partition boot record | `src/format.c:WritePBR` calls ms-sys `write_fat_16_fd_br` or `write_fat_32_fd_br`, with `bKeepLabel=0`, then writes the physical drive ID. FAT32 repeats at sector 6. | Compile the unchanged `src/ms-sys/fat16.c`, `fat32.c` and `partition_info.c`, supplying their small I/O interface separately. CP19a verifies this compiles and executes on Linux without `ms-sys/file.c`, Win32 types or a Windows compatibility runtime. |
| DOS payload | `src/dos.c:ExtractFreeDOS` obtains 27 resources, copying COMMAND.COM and KERNEL.SYS to the root and 25 support files to LOCALE. Root files have hidden/system attributes. | CP19c2 shares the ordered manifest, destinations and progress markers through `src/freedos_files.inc`; Windows consumes its resource IDs and Linux CMake embeds the same files. Native copying uses a retained private FAT directory and kernel attribute ioctls. |
| DOS startup files | `src/dos_locale.c:SetDOSLocale` chooses keyboard/code page and writes CONFIG.SYS/AUTOEXEC.BAT. | CP19c1 shares DOS tables, fallback rules and generated content in `src/dos_locale_policy.c`. CP19c3a publishes those bytes through the native copier's exclusive file creation, sync and readback. CP19c3b uses an explicit Qt choice on Linux. Windows retains its host queries. |
| Progress and completion | `InitProgressState` reserves six discrete file-copy slots for BT_FREEDOS: one before extraction and five milestones in ExtractFreeDOS. | Use the existing shared progress state and one authorized helper. Sync files, filesystem and disk, unmount the private mount and wait for helper exit before successful READY. |

Ordinary FreeDOS partition placement can reuse `GetPartitionStartOffset` and
`GetMainPartitionSize`. Old-BIOS fixes, remaining geometries, full formatting and
other advanced options remain in the full-parity backlog; they are not silently
removed from scope by the first ordinary-MBR checkpoint.

## Payload inventory

The resource list in `ExtractFreeDOS` matches all 27 active FreeDOS entries in
`src/rufus.rc` and all referenced files exist: **400,604 bytes** in total.
The inventory is recorded with source and binary SHA-256 hashes in
`.rufus-test-artifacts/cp19a-resource-manifest.json` in the parent workspace.

`res/freedos/readme.txt` identifies the existing files as coming from FreeDOS
1.4. It specifically records that Rufus uses KERNL386.SYS as KERNEL.SYS with
FORCELBA enabled by setting byte offset `0x0d` to `0x01`; the checked-in file has
that value. Use this exact file rather than substituting a distribution kernel
or a separately installed `sys` program. No download or new runtime FreeDOS
dependency is needed to obtain these resources.

Rufus's existing attribution in `src/license.h` covers the FreeDOS and ms-sys
components. Preserve that notice and the bundled provenance when packaging the
Linux resources. This audit inventories the repository's payloads; it does not
reconstruct or independently qualify their upstream builds.

## Boot-record write contract

The native probe compiles the original FAT routines and implements only
`read_data`, `contains_data` and `write_data` over a bounded real FILE memory
stream. It does not reproduce the Windows FAKE_FD structure or link its I/O
implementation. The same separation can serve a production adapter that first
reads and validates a retained partition prefix, builds the changed bytes in
memory, and writes, flushes and reads back that bounded prefix through existing native
target I/O. Avoid a general Windows API compatibility layer.

The measured writes are:

| Record | Embedded code ranges, end exclusive | Other original changes |
| --- | --- | --- |
| FAT16 | `[0, 11)`, `[0x3e, 512)` | With `bKeepLabel=0`, set the boot-sector label at `0x2b` to `NO NAME` padded to 11 bytes. Set drive ID at `0x24` to `0x80`. |
| FAT32 | `[0, 11)`, `[0x52, 0x3e8)`, `[0x3f0, 1536)` | With `bKeepLabel=0`, set the boot-sector label at `0x47` to `NO NAME` padded to 11 bytes. Set drive ID at `0x40` to `0x80`. Repeat for the backup starting at sector 6. |

FAT32 modifies more than its first 512 bytes. It rebuilds the FSInfo/reserved
sector content while preserving the eight bytes holding the free-cluster count
and next-free hint at `0x3e8..0x3ef`. Preserve the BPB except the original explicit
drive-ID/boot-label changes. The filesystem's directory volume label is a
separate entry: do not rewrite it just because Windows uses `bKeepLabel=0` here.
Test the final label through independent FAT directory readback.

The CP19b native adapter validates the FAT kind, 512-byte sector geometry,
reserved-sector coverage and expected backup/FSInfo locations before any write.
It prepares both FAT32 copies first, rejecting a mismatched backup BPB/signature
before modifying the primary. It preserves surrounding sectors, descriptor
ownership and position. Flush/readback failures and cancellation propagate to
the caller, with a separate flush error retained after a prior write error.

`linux/platform/fat_boot.c` (generalized in CP21b) reuses `CheckFat16Image`/`CheckFat32Image` for
request, descriptor and filesystem validation. The original writers receive
bounded real `fmemopen` streams, with one spare byte for stdio's terminating NUL.
Only the resulting 512-byte FAT16 prefix or two 1536-byte FAT32 prefixes reach
the target through positional I/O. This small C adapter does not need a second
FAT parser, a temporary source file, process execution or a new job framework.
Exact prefix readback and the existing geometry reader run after `fsync`; the
geometry reader expects the original writer's `NO NAME` boot label at that point.
Independent directory/mtools tests confirm the selected directory label remains.

CP19d2b extends that same implementation to a retained whole-disk descriptor,
using the existing native FAT validators for block identity, capacity and
geometry. The helper checks the MBR before PBR installation and reuses its
formatter claim handoff, private FAT mount, six copy slots and cleanup lifecycle.
The mounted root descriptor is borrowed directly; the target still owns it.
Both bundled-file and selected-startup writers run before shared finalization.
An earlier file-sync error remains visible even if the final filesystem flush
succeeds. No second authorization, host locale query or Windows API emulation
is introduced.

Independent native-media checks accept only fsck.fat's expected NO NAME BPB /
directory-label discrepancy, checking all other output for unexpected repairs.
That label discrepancy follows the original writer's bKeepLabel=0 behavior;
rewriting it would change the original PBR contract. The chosen directory label
remains intact, and both original FAT32 boot sectors match after private unmount.

One original error-path issue needs explicit handling: the FAT32 branch of
Windows `WritePBR` breaks its loop on a failure but then returns TRUE. The
unchanged ms-sys functions themselves correctly return failure, as the probe
checks at every write position. The Linux caller must preserve those failures.
Do not copy the unconditional success, and do not claim the Windows wrapper was
fixed or runtime-tested by this checkpoint.

CP19d2c boots two unchanged helper-created sticks through SeaBIOS: original
Rufus MBR/FAT32 with RO/852 and original Windows 7 MBR/FAT16 with US/437. The
bundled FreeCOM prompt executes VER/ECHO, and the expected startup selection is
visible. No Linux kernel/initrd is passed to this second VM and no DOS files or
startup commands are added to the media. Snapshot boot preserves the independently
verified disk hash. This supplies representative boot evidence for these original
payloads; it does not qualify all firmware, locale or advanced BIOS combinations.

## Locale split

There are two distinct inputs and a reusable DOS output policy:

- Windows `get_kb()` queries the keyboard layout name and maps it through its
  existing Windows IDs, with language/US fallbacks. `GetOEMCP()` and the UTF-8
  fallback query supply the requested OEM code page. Keep these host calls in
  the Windows wrapper; Linux should not manufacture Windows keyboard IDs.
- The DOS keyboard-to-driver lists, human-readable labels, code-page-to-EGA
  lookup, driver default code pages and `850 -> 858` FreeDOS upgrade are common
  policy. Extract them once, preserving the current order and fallback behavior.
- File content is common too. US keyboard plus code page 437 produces only
  AUTOEXEC.BAT. Other combinations produce the original FreeDOS language menu
  with a five-second default and a US/437 choice, followed by the original
  display/mode/keyb commands. Preserve bytes, DOS path separators and CRLF
  output: Linux text-mode stdio does not perform Windows's newline translation.
  The menu also uses literal OEM box-drawing bytes; a UTF-8 rewrite is not an
  equivalent result.

**User decision 2026-09-29 supersedes Linux host detection:** let the user choose
the FreeDOS keyboard and code page explicitly, initially US/437. Present the
choice in the unprivileged Qt flow before authorization or writes, and freeze
accepted, bounded DOS values with the job. Cancel aborts preparation. This
intentional Linux UI difference avoids ambiguous desktop settings and makes the
DOS result reviewable; Windows retains its existing automatic host selection.
This is unrelated to copying Windows regional settings, which stays disabled
on Linux as previously agreed.

CP19c1 implements the common policy in `src/dos_locale_policy.c/.h`.
`SelectDOSLocale` returns a selection with static strings and explicit fallback
flags. `WriteDOSConfig` and `WriteDOSAutoexec` use standard binary FILE streams
and emit the original content with explicit CRLF. The Windows wrapper retains
its keyboard/OEM query bodies unchanged and owns opening/closing the files;
the Linux tests supply explicit DOS inputs, not inferred host settings.
Both FreeDOS and MS-DOS use the shared module. No second Linux table/generator
or GUI callback layer is introduced.

Three small corrections accompany this extraction, shared by both platforms:

- The FreeDOS EGA12 mapping now names `ega12.cpx`, matching the bundled file.
  The original omitted the extension for code pages 30003, 30029, 30030 and
  58335, generating a command that referenced a nonexistent payload.
- Driver-supported keyboard IDs without a friendly label display their
  canonical DOS ID. Unknown code-page descriptions use `Unknown`. This avoids
  the original assertion/NULL formatting while preserving driver and fallback
  selection; it does not invent a different keyboard layout.
- The Windows file wrapper checks writes and close errors, including buffered
  failures. It must not report successfully written startup files after a full
  filesystem or other I/O error. It retains CONFIG-before-AUTOEXEC ordering and
  skips CONFIG for the original US/437 case.

The frozen pre-extraction oracle matches 468 FreeDOS/MS-DOS output cases, except
the four intended EGA12 filename corrections. Separate tests cover canonical
labels, fallback flags, driver order, stream/close errors and invalid arguments.
The Windows build manifests include the common source/header; actual Windows
compilation and execution remain deferred, as agreed.

Split content generation from file creation and logging. Both platform wrappers
can check writes and close errors while using the same generated bytes. Keep
the extraction resource manifest and DOS policy compact; there is no need for
a plugin registry, a general bootloader framework or per-file callback classes.

CP19c2's `CopyFreeDosFiles` uses the shared resource manifest and copies only the
27 payload files; startup-file publication and user selection are separate.
It requires an empty, exclusively borrowed directory on kernel FAT,
uses retained directory descriptors with exclusive file creation, and reuses
the ISO extractor's empty-directory guard through `target_io`. Each payload is
checked after file sync; directory metadata is flushed even after cancellation
or failure. Completed/partial files remain on error and the job is incomplete.

The root files get hidden/system bits through `FAT_IOCTL_GET_ATTRIBUTES` and
`FAT_IOCTL_SET_ATTRIBUTES`, preserving other bits and reading attributes back.
Native qualification in a small Linux guest passes on FAT16 and FAT32 with the
same vfat mount options as the existing helper. Host-side mtools/fsck readback
confirms all payload bytes and attributes after unmount. The two disposable
volumes and guest boot files are removed, leaving logs/hashes. This tests file
copying on kernel FAT, not FreeDOS bootability or a complete authorized helper.
No new authorization or mount manager is introduced.

CP19c3a adds `WriteFreeDosStartupFiles` to that same component. It takes explicit
DOS keyboard/code-page inputs, selects the unchanged shared policy and renders
into a standard memory FILE stream. Only the policy's static strings enter the
writers. The resulting bytes use the payload copier's existing exclusive writes,
file sync, readback and failure handling. CONFIG precedes AUTOEXEC; the original
US/437 case writes only AUTOEXEC. Neither startup file gets hidden/system bits,
and publication adds no progress milestone. The caller retains final filesystem
and device sync, private unmount and helper-exit responsibility.

The native wrapper refuses either existing startup name before writing anything,
including an otherwise skipped CONFIG. Windows uses `fopen(..., "wb")` after
formatting and extraction. On Linux an existing startup entry contradicts the
fresh-target contract; overwriting it or retaining stale CONFIG directives could
change the generated boot behavior. Fail instead, preserve the existing entry,
and use `O_EXCL` to close the check/create race. Failure or cancellation after a
started write leaves completed/partial files and syncs root metadata; it never
reports the job ready. Tests cover regular files, directories, hard links,
symlinks, dangling links and creation after preflight.

The host-discovery investigation was closed by that user decision. Qt's
[QInputMethod locale](https://doc.qt.io/qt-6/qinputmethod.html#locale-prop)
describes the input locale; it does not specify an XKB layout/variant.
Systemd's [locale1 interface](https://github.com/systemd/systemd/blob/main/man/org.freedesktop.locale1.xml)
exposes system keyboard configuration, not the currently selected desktop group.
The [XKB configuration model](https://xkbcommon.org/doc/current/xkb-intro.html)
also distinguishes layouts from variants and options. Consequently, substituting
`QLocale::system()` or the first configured layout for the active keyboard is not
a qualified host adapter. No desktop-query code or new host-to-DOS mapping
table was added. The investigated DOSBox Staging mappings were not imported.

**CP19c3b** adds a small Qt Widgets dialog with keyboard and code-page selectors.
Two read-only enumeration functions expose the existing FreeDOS driver IDs and
available EGA code pages from the shared policy; no copy of those tables enters
Qt. Labels also come from that policy. Only final code pages are shown, so 850
appears as 858, matching Rufus's existing upgrade. UTF-8 and code pages lacking
an EGA payload are absent. Unknown restored selections require correction
rather than silently falling back; changing the keyboard preserves the explicit
code-page selection. The lists describe available drivers/fonts, not a newly
introduced keyboard-to-code-page compatibility matrix or automatic recommendation.

The dialog defaults to US/437 independently of the Linux locale. A non-default
choice retains the original generated language menu with its US/437 alternative;
US/437 retains the no-menu case. CP19d supplies bounded request fields and the
complete helper route. CP19e connects the chooser to START before the existing
single authorization, freezes accepted values, and tests Cancel and stale-device
refusal in the normal window.

## Checkpoints and acceptance

1. **CP19a, complete:** source/resource audit and test-only native compilation of
   the existing FAT boot-record writers. Four FAT16/FAT32 × keep-label cases
   preserve the independent BPB/FSInfo boundaries and guard bytes. Each of the
   16 write positions is failed in turn. Normal and ASan/UBSan/leak runs pass.
   This is a memory-prefix probe, not formatted-media or firmware qualification.
2. **CP19b, complete:** a bounded native FreeDOS FAT boot-record adapter using
   those original routines. Normal and ASan/UBSan/leak runs pass 100 cases on
   formatted FAT16/FAT32 regular files, plus memory bounds/non-file rejection.
   Primary/backup bytes, BPB/FSInfo preservation, directory label, outside bytes,
   extent guards and failure/cancellation paths are checked. Fixtures are removed.
   No device-helper or GUI enablement yet; this is not BIOS boot qualification.
3. **CP19c, component work complete:** CP19c1 completes shared DOS policy/content and native
   byte comparisons, retaining Windows host queries in its wrapper. CP19c2
   shares the payload manifest and qualifies native file copying/FAT attributes
   on kernel FAT16/FAT32. CP19c3a publishes startup files through the shared
   writers and qualifies retained-directory I/O with explicit DOS inputs.
   CP19c3b implements the explicit user-choice dialog in place of host detection.
   Keep Windows's existing build system when adding shared sources. Do not infer
   a DOS layout from locale language or pretend UTF-8 is an OEM code page.
4. **CP19d, complete:** CP19d1 extends the native MBR writer with original Rufus
   and Windows 7 boot code, retaining the disk signature and active FAT partition.
   CP19d2a establishes the strict request and six-step progress contract.
   CP19d2b removes its temporary refusal and joins formatting, boot records,
   private mounting, payload/startup copying and final sync/exit in one helper.
   All four FAT16/FAT32 × MBR-code variants pass simulated-device failure and
   success cases with independent readback. CP19d2c boots two representative
   helper-created disks under SeaBIOS to the DOS prompt and verifies commands.
   Compact logs/hashes/screenshots remain; disposable disks/guest trees are removed.
5. **CP19e, complete:** the normal FreeDOS Qt choice follows MBR/BIOS/FAT
   eligibility and defaults, retaining prior ISO selections for switching back.
   Functional tests cover labels, explicit locale choice and Cancel, frozen
   requests, original discrete progress, sync/cleanup failure and waiting for
   helper exit before full READY. MS-DOS and other boot choices remain separate
   until their own backends are ready.

These checkpoints were completed separately. Keep the user's small-checkpoint
and disk-cleanup rules for the remaining parity work.
