# ISO read options and timestamps

CP69 / Preview 38 adds the original advanced ISO shortcuts to the ordinary Qt
flow. Options are frozen before preflight and authorization.

| Shortcut | Default | Effect |
| --- | --- | --- |
| Alt+I | ISO support enabled, session | Reanalyse the selected eligible image with ISO support disabled/enabled; a recognized hybrid can use the existing DD route. |
| Alt+J | Joliet enabled, session | Select Joliet eligibility for ISO9660 scanning and extraction. |
| Alt+K | Rock Ridge enabled, session | Select Rock Ridge eligibility for ISO9660 scanning and extraction. |
| Alt+S | Size checks enabled, session | Bypass the projected ISO-size heuristic; explicitly permit MS-DOS 64 KiB clusters. |
| Alt+T | Disabled, saved `PreserveTimestamps` | Preserve ordinary copied ISO9660/UDF access and modification times. |

Changing I/J/K rescans an eligible selected ISO. Manual labels and filesystem
choices survive the rescan when still eligible. The busy guard keeps active jobs
immutable. Status and session log report the switch. UDF keeps its original
priority over a companion ISO9660 tree; J/K do not change UDF names.

## Original behavior and selected trees

Windows `src/rufus.c` implements these switches; `src/iso.c` prefers Rock Ridge
for metadata scanning, uses the base extension mask for standalone boot-content
reads, and usually uses Joliet for extraction. Rock Ridge long filenames or
Rock Ridge symlinks require the Rock Ridge extraction tree. The shared
`GetIsoExtensionMask` implements those same choices for both platforms. Linux
retains the chosen extraction view for subsequent preflight/content/copy access.
Ordinary ISO files are read from each visited directory record, as in Windows,
so duplicate Joliet file names cannot redirect the second file to the first
record. The range reader still bounds every read.

Windows J/K changes do not immediately rescan the already selected image. Linux
rescans it so displayed policy and the helper's independently reanalysed source
describe the same extensions. The helper cannot rely on caller-supplied metadata.
This is a deliberate consistency requirement of the native privilege boundary.
Existing native raw-suffix dispatch (`.img`, `.usb`, `.wic`) is unchanged; this
batch does not establish ISO probing parity for an ISO renamed to those suffixes.

## Timestamps

Windows copies ISO timestamps and UDF access/modification/creation times and
logs optional setting failures. Linux applies access/modification times to the
opened destination descriptors. ISO times use the original `mktime` semantics;
UDF uses the shared access/modification time readers. Creation/birth time uses
the native filesystem behavior: Linux has no portable setter. FAT access time
has date precision and modification time has two-second precision.

Linux sets directory timestamps after their children, because child creation
would otherwise immediately change the directory modification time. Ordinary
file times are applied before config rewriting, preserving Windows ordering;
rewritten, generated, split-WIM and supplementary ZIP files retain their existing
behavior. Optional metadata failures are logged. Mandatory filesystem/device
sync, cleanup and helper exit still determine success and full green READY.

## Size checks

The enabled START heuristic refuses projected ISO contents more than 4 KiB
larger than the device, matching Windows. Alt+S skips that heuristic. The helper
still requires actual content, filesystem structures and writes to fit: this
option never permits writing beyond the physical device.

Windows also uses size checks to exclude MS-DOS 64 KiB clusters. Linux retains
that default restriction and exposes the explicit override. A strict frozen
`skipSizeChecks:true` is accepted only for a device MS-DOS job. It is not inferred
from a host setting. Such DOS media may not boot on all machines; the override
does not establish compatibility of MS-DOS applications with 64 KiB clusters.

## Qualification

Focused actual ISO9660/UDF tests cover extension selection, long names, file and
directory timestamps, protocol admission, saved/session switches, frozen scans,
manual label retention, ISO/DD rescanning and mandatory bounds. One combined
normal/sanitizer disposable guest route exercises the real helper and ordinary
Qt controller for MS-DOS/FAT16 64 KiB, ISO9660/FAT32 and UDF/NTFS. Independent
kernel readback checks payloads, timestamps and the DOS BPB. One representative
UEFI:NTFS/native EFI fixture boot reuses the shared writer boot evidence. These
simulated media checks do not establish every physical PC's compatibility.
The CP69 receipt records final results and any initial fixture repairs.

The complete Windows parity goal remains active. Other ISO transformations,
architectures, legacy payloads and filesystem/device/distribution rows remain in
the [parity backlog](linux-feature-parity.md).
