# Standalone MS-DOS and ReactOS boot records

CP64 adds **MS-DOS** to the normal Boot selection list and **ReactOS** under
advanced drive properties. Both use the original MBR/FAT16/FAT32 eligibility,
formatting and boot records. MS-DOS excludes 64 KiB clusters by default; CP69 adds the original explicit
Alt+S override. See [ISO controls and size checks](linux-iso-options.md).
The ordinary confirmation and single authorized helper cover the complete job.
Successful READY follows file/device sync, private-mount cleanup and helper exit.

## MS-DOS

Rufus asks for an explicit keyboard and code page from its original MS-DOS
tables, starting at US/437. It does not infer Linux desktop or regional settings.
Other choices add the original DOS boot menu with a US/437 fallback. The
existing FreeDOS choice keeps its own driver lists and startup behavior.

The required `diskcopy.dll` is downloaded from Microsoft's fixed symbol-server
URL only after the original Yes/No prompt; No is the default. Valid cached data
is reused from the normal or portable application files folder. The original
size and SHA-256 are mandatory. The resource is read as data and never loaded
as a library or executed. Microsoft files are not included in the Linux packages.

The native adapter extracts the same 13 files from the known FAT12 floppy
resource, keeps support files under `LOCALE`, and applies Rufus's original
WinME real-mode patches to `COMMAND.COM` and `IO.SYS`. Shared
`src/msdos_files.*` supplies the fixed resource identity, file names and patches
to both Windows and Linux. The cached DLL remains unchanged.

Selected bytes are retained, privately staged and checked again by the helper
before target writes. Original FAT attributes and source access/modification
times are applied through the mounted filesystem. Creation time follows the
native FAT file-creation behavior: Linux's file API has no birth-time setter.
This small metadata difference avoids mutating FAT directory entries behind
the mounted kernel filesystem. Invalid attributes, file copy, sync or cleanup
cannot produce successful READY.

Windows normally notices an invalid cached resource during extraction, after
formatting. Linux validates it during unprivileged selection and again before
target acquisition, so a bad cache refuses before destroying existing media.
This explicit difference protects target data. Atomic cache replacement and
readback also avoid accepting an interrupted or changed download.

## ReactOS

The original standalone option prepares the ReactOS MBR and FAT partition boot
records. It does not download or copy a ReactOS installation. Supply the desired
loader/system files separately, as with the original Windows Rufus option.
The FAT32 writer includes the original sector-14 tail and its sector-20 backup;
existing Windows boot-record tails at sectors 12/18 remain unchanged.

The native writer preserves filesystem geometry and checks the resulting
records with the original ms-sys code, including readback and sync. The original
MBR choice and eligible FAT layouts come from the shared Rufus format policy.

## Evidence and scope

One focused normal/sanitizer group covers the original patches, fixed resource,
cache/HTTP consent and errors, strict protocol and format constraints, original
locale lists, actual ReactOS records and existing process/window/FreeDOS cases.
The combined guest route uses ordinary Qt creation for all four boot/filesystem
choices, independent mtools-derived DOS payload hashes, original attributes,
explicit US/Romanian startup, released claims and one authorization per job.
A changed-resource control must leave the complete disk unchanged.

The representative ReactOS BIOS handoff uses a bare 32-KiB native fixture named
`FREELDR.SYS` and verifies its tail after multi-cluster loading. Its entry
contract follows the [upstream boot assembly](https://github.com/reactos/reactos/blob/master/boot/freeldr/bootsect/fat32.S).
No Microsoft or ReactOS application or installation runs in qualification.
Existing shared helper fault, polkit and agreed Windows customization evidence
is reused. The focused group passes246 real cases per normal/sanitizer build;
the one full Qt gate passes all2,200 real cases with five existing optional skips.
Both combined guests pass. Native/generic package inspection verifies the
installed interfaces and that no Microsoft resource is bundled.
Exact commands, outcomes and limits are in the CP64 receipt under
`../.rufus-test-artifacts/cp64/`.

MS-DOS media needs legacy BIOS or UEFI-CSM. Modern firmware without that mode
cannot boot this DOS path. Original Windows image download selection, application
updates, localization and all other unfinished parity groups remain active.
