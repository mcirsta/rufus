# Legacy Windows, ReactOS and Kolibri ISO routes

CP75 / Preview44 delivers B4: I17, I18 and I19. Source eligibility follows this
checkout's original `GetAllowedFileSystems`, `GetBootInstallType`,
`GetSecondaryBootType`, `GetMasterBootRecord` and `GetPartitionBootRecord`.
The source scan is repeated in the authorized helper before target access.

## Windows and PE

Ordinary BOOTMGR creation no longer requires a recognized installation archive
or normalized Windows version. Vista keeps its original unknown normalized
version. BOOTMGR/WIM PE without an install archive, unknown archive metadata and
Windows 7/8/8.1/10/11 use the same source dispatch. Unknown version metadata does
not acquire modern customization options.

Windows BIOS offers FAT16/FAT32 only when the original Alt+E policy allows it;
plain BIOS retains NTFS. Older UEFI sources can use eligible FAT16 too. Each
selected fallback EFI application is checked, and the original image-1 Windows
EFI repair remains mandatory for sources that need it. No alternate archive or
loader is substituted after a repair error.

A complete i386, amd64 or minint setup family selects the existing SetupWinPE
preparation on a BIOS target, including eligible BOOTMGR, EFI, install-archive,
Syslinux and GRUB combinations. Partial families remain subject to their actual
source requirements. SetupWinPE preserves SIF encoding, uses the shared loader
patch, retains detector ownership and verifies an existing root loader against
its required transformed bytes. Other ordinary root loaders remain copied.

Primary and secondary installation are independent. Without Alt+E, a selected
Syslinux route retains its PBR through WinPE file preparation. GRUB2/GRUB4DOS
secondary resources follow the original precedence. Required GRUB2 resources are
selected and frozen before authorization; the original embedded GRUB4DOS SBR
stays inside the helper. NTFS reserves UEFI:NTFS whenever the source policy
requires it, including PE sources without an existing BIOS BOOTMGR.

Original Windows additionally invokes its Fmifs CheckDisk service after NTFS ISO
creation. Linux retains the qualified native PBR-before-mount order, checked
writes, filesystem/device sync and private unmount. The NTFS/EFI media case
independently checks its resulting clean volume, files and boot partitions.
These checks do not implement Windows CheckDisk or repair arbitrary corruption;
full Windows API execution and installed WinPE/AIK behavior remain unqualified.
There is no retry through another provider after an I/O or corruption error.

FAT16 uses the original standard PBR and physical drive 0x80. That record expects
IO.SYS/MSDOS.SYS; a BOOTMGR-only FAT16 source does not gain a different BIOS loader.
FAT32 uses the original PE or NT record selected by the report. Final legacy
allocation/drive fixups and partition flag 0x81 are written after private-mount
cleanup. The flag is independent of the selected original MBR code.

## ReactOS ISO creation

A ReactOS loader selects Syslinux even when the source has no Syslinux config.
That case uses the embedded 4.07 pair and original `mboot.c32`. An actual source
Syslinux version keeps the existing version/resource selection rules.

The helper writes the original ReactOS root config and mboot before extraction;
ordinary source files can overwrite them in the original order. Extraction's
empty-directory rule admits exactly those two byte-checked prepared files.
The source's freeldr/setupldr bytes are copied through ordinary extraction.
The final MBR follows the actual source report independently of the Syslinux PBR;
a source without Syslinux configs uses the saved Rufus/Windows 7 MBR choice.
The existing native Syslinux config-lookup checks still require that the native
and original loader resolve the same config; incompatible layouts fail prewrite.

## Kolibri ISO creation

The FAT32 route uses the original Kolibri MBR and PBR. FAT16 PBR installation
remains refused before writes, as in the original writer. NTFS uses the original
ordinary NTFS PBR/MBR selection and source-dependent UEFI:NTFS reservation.

After extraction the helper copies `HD_Load/USB_Boot/MTLD_F32` to root
`MTLD_F32`, checks its bytes and sets and reads back actual hidden/system DOS
attributes. This applies to source dispatch mixtures too. FAT uses its attribute
ioctls; NTFS uses the selected provider's DOS attribute interface. These flags
are separate from the ordinary autorun files used by extended labels.

Generated/copied boot resources are included in preflight space accounting.
Reserved loader paths must be ordinary files. Source identity, cancellation,
file and filesystem sync, private unmount and joined helper exit remain part of
successful READY. The GUI remains unprivileged and authorizes a complete job once.

## Qualification

Six focused component suites pass in normal and sanitizer builds: 1,897 substantive Qt cases, seven existing optional Microsoft-resource skips, plus 244 ISO/UDF extraction cases. All fourteen new legacy resource/protocol/source cases pass. One combined native route per build creates and reads back five jobs: legacy PE FAT16, unknown-version Windows FAT16 dual, ReactOS FAT32, legacy PE NTFS with EFI/UEFI:NTFS, and Kolibri FAT32. Exact MBR/PBR/source/prepared bytes, actual hidden/system FAT attributes, final NTFS dual layout/clean flags, one authorization and joined READY pass. Three representative firmware handoffs pass: native FAT16 EFI, original Syslinux/mboot ReactOS and Kolibri PBR. One full nine-suite Qt run plus focused normal/sanitizer repairs qualifies 2,214 substantive cases with 2 existing optional skips and no outstanding failures. The full run found twelve failures in one obsolete root-loader-refusal group (including one fixture startup failure); all twelve rows pass after its acceptance expectations are updated. Product sources remain unchanged after the full gate. The earlier focused UI group had two failures; one fixture expectation and the Kolibri phase contract were repaired, and all six B4 repair rows pass in normal and sanitizer builds. Existing unchanged helper faults and installed-Windows/customization evidence are reused.

The compact evidence is under `../../.rufus-test-artifacts/cp75/`. The delivery
receipt records the final grouped component, ordinary Qt and combined normal and
sanitizer media results. The generated corpus covers original dispatch classes;
it does not add a release-by-release installed-Windows/customization matrix.

The combined media route creates and independently reads back five real guest
block-device jobs: legacy PE FAT16, unknown-version Windows FAT16 dual, ReactOS
FAT32, legacy PE NTFS with EFI/UEFI:NTFS, and Kolibri FAT32. It checks original MBR/PBR bytes, final active flags,
prepared/copied files, Kolibri attributes, one authorization and READY after exit.
Representative handoffs exercise the shared FAT16 EFI writer, original ReactOS
Syslinux/mboot chain and original Kolibri PBR with generated loader fixtures.
Physical media and complete operating-system installation remain unqualified.
