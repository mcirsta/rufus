# Native device discovery

CP65 adds running-application disk exclusion and existing extended labels to
read-only discovery. Windows references are `src/dev.c` (the application/system
letter exclusion and Multiple Volumes caption), `src/drive.c:GetDriveLabel` and
`src/parser.c:get_token_data_file`. Selection and helper target validation remain
independent of display content.

## Running application

Windows removes a disk containing the application directory or system drive.
The Linux client captures the executable and its directory's backing devices
from descriptors and sysfs. Partition parents, device-mapper/RAID slaves,
file-backed loops and Btrfs members are included. Every positively identified
whole disk is excluded even with List USB Hard Drives enabled. This is target
protection, not inference of user preferences. The existing system/home/source
and write-time checks still apply.

Anonymous or unresolved FUSE/container mappings do not identify an arbitrary
USB disk. Only positive native backings are hidden; broader backing/host/device
qualification remains open. The resolver is bounded to 64 devices and 16 levels
and handles cycles without repeatedly visiting the same device.

## Extended labels and captions

Windows reads the first assigned volume's `autorun.inf` before falling back to
its filesystem label. Without a drive letter it uses the first filesystem label.
Linux uses existing UDisks mountpoints as the corresponding accessible volumes;
it does not mount media or request privileges for a label. Mounted volumes are
ordered by partition offset because Linux has no drive-letter order. An
unmounted disk retains the first filesystem's IdLabel. Whole-disk filesystems
use their own metadata.

The original first-nonempty `label` token parser is shared with Windows. It
keeps case-insensitive tokens, spaces/tabs, optional quotes/XML syntax, literal
unquoted semicolons, trailing spaces and no section restriction. The native
reader follows the Windows `ccs=UNICODE` mode: default UTF-16LE or BOM-selected
UTF-8/UTF-16LE. See Microsoft's [Unicode stream documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fopen-wfopen?view=msvc-170).
It reads metadata only; autorun commands and icons are not executed or loaded.

A worker handles one immutable discovery snapshot at a time and coalesces later
snapshots. A mount/content change or service failure invalidates older results.
Only label/count fields can be applied to the current matching selection ID.
Physical eligibility, read-only transitions and removal publish immediately.
Labels are display facts and never part of a disk's identity or permission to
write. The selected label also supplies the existing default volume-label editor
when the user has not entered one.

The list and destructive confirmations show the extended label, device path
and size. Empty labels fall back to the device name. Multiple distinct mounted
volumes show **Multiple Volumes**; aliases of one filesystem count once.
Windows's `EFI/Rufus/ntfs_x64.efi` UEFI:TOGO auxiliary volume is omitted from that
count when positively found. Unreadable volumes retain their native label.

## Native metadata safeguards

Windows uses a 1024-wide-character line buffer and a MAX_PATH UTF-8 display
buffer. Linux retains the line-token bound and valid Unicode up to that bound,
without splitting a UTF-8 character at the Windows display-buffer boundary.
This avoids replacing part of a multibyte character in the Qt caption.

Linux also bounds the file to 64 KiB and its case-insensitive directory search
to 4096 entries. It refuses ambiguous case variants, links, nonregular files,
invalid encodings/NUL/control labels, changed metadata and mismatched filesystem
identities. Such metadata falls back to IdLabel. These differences avoid
unbounded/removable-media work, misleading control text and following an
image-controlled link into a different location. Directory enumeration uses an
independent descriptor and does not move a caller's directory offset. They do
not change the accepted source image or its content.

## Qualification

One grouped normal/sanitizer component and private-D-Bus/UI batch covers parser
syntax/encoding, mounted-file safety, backing graphs, label mount/unmount
refresh, stable selection and captions. One combined networkless guest per build
checks real mounted FAT16, native NTFS and exFAT labels as UID 1000, asynchronous
read-only listing and exclusion when the executable is on the USB directly or
inside an ext4 loop image stored there. Only guarded disposable guest media is
written to construct fixtures. No host device writes or mounts are performed.
The writer/helper/polkit/boot contracts are unchanged; their established fault,
readback and boot evidence is reused. One full nine-suite Qt gate and package
inspection complete this batch. Exact results are in CP65's checkpoint receipt.


## Advanced device classes (CP71 / Preview 40)

The original Windows controls are now connected to native discovery and job
validation as one group:

| Control | Default and lifetime | Native behavior |
| --- | --- | --- |
| Alt+G | Enabled; saved inverse `DisableVHDs` | List recognized attached virtual disks. Disabling also hides VMware disks. |
| Alt+W | Disabled; saved `EnableVMDKDetection` | Include recognized VMware disks; Alt+G still applies. |
| Ctrl+Alt+F | Disabled; session only | Include non-USB removable disks, enable List USB Hard Drives, then restore its previous value when toggled off. |

The Windows reference is `src/dev.c:IsVHD` and the Alt+G/Alt+W/Ctrl+Alt+F
handlers in `src/rufus.c`. Linux reads the actual block-device vendor/model,
transport ancestry, removable flag, capacity and disk sequence from sysfs.
Separate native SCSI fields replace the padded Windows hardware-ID strings.
Microsoft, Arsenal, KernSafe and bhyve virtual models and the original VMware
model prefix are recognized; an arbitrary display name containing “virtual”
does not qualify. Native MMC cards use the existing shared 500-GiB default card
limit. Ordinary fixed internal disks remain excluded.

UDisks marks internal-bus disks with HintSystem even when they are unused
secondary virtual disks. For these positively recognized native classes, the
helper's existing kernel mount/backing checks determine protected use.
HintIgnore still excludes a target. Native protection is mandatory for every
new class, including an empty UDisks mount inventory. System, home, application,
source and active layered-device protections remain in force. No switch grants
permission to erase a system disk.

The selected class is part of discovery identity and the frozen device request.
The strict protocol omits the default USB class and accepts only one known class
for a native device. Preflight and the authorized helper independently reread
that class; changing the setting cannot turn an old selection into a different
disk. Disabling a class removes and invalidates its selection. Read-only media
can use the capture path and continue to refuse destructive jobs.

The original PortableBaseLayer label exclusion and small Bitdefender virtual
disk exclusion also apply. CP73 adds positively identified file-backed loop/NBD
disks and their drive-less UDisks objects under Alt+G; see
[device/source operations](linux-device-source-operations.md) for backing checks,
optical capture and exclusions. Native identification of Microsoft Dev Drives,
other virtual providers and physical hotplug/sector qualification remain open.
The native port does not advertise support for every Linux virtual block provider.
The Windows VHD/VHDX image-file readers and parent/log support are separate,
already implemented features.

Qualification uses generated sysfs cases, the actual shortcut slots and settings,
strict protocol cases and private-D-Bus discovery/invalidation checks. One
networkless guest per build groups three real SCSI device classes through
ordinary Qt formatting, independent kernel readback and read-only capture
geometry/claim checks. A mounted home on a virtual target with an empty UDisks
mount list must fail before writes and retain the whole-disk hash. Existing
writer/boot, authorization and helper-fault evidence is reused because write
semantics are unchanged. The full nine-suite Qt gate runs once for the batch.
