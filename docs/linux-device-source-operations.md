# Device and source operations (B2)

CP73 / Preview42 (2026-10-24) delivers I06–I09, I11, A01 and A04.
The normal/sanitizer focused groups each pass 185 substantive cases. A combined
native guest route per build verifies all distinct operations plus one mapped-parent
restore, full readback and BIOS handoff. One nine-suite Qt gate passes 2,210
substantive cases, five existing optional skips and zero failures. See the
[checkpoint record](testing-and-checkpoints.md#cp73--device-and-source-operations-preview42-2026-10-24)
and [completion register](linux-parity-register.json). Physical devices and the
Windows desktop API have not been executed for this batch.

## Desktop commands

| Command | Action |
| --- | --- |
| Alt+O | Select the first available optical source and save its raw bytes to ISO. |
| Alt+C | Reset the selected USB device and refresh discovery. |
| Alt+P | Toggle an ESP to data, or restore an ESP previously toggled by Rufus. |
| Alt+, | Enable/disable Rufus's exclusive kernel device claim for this session. |

All four commands are disabled during another job. Device commands use the
existing authorized helper and one authorization for the complete operation.
The GUI remains unprivileged. Successful READY follows sync, cleanup and helper
exit. A normal write still leaves the USB connected, without automatic eject.

### Optical capture

Optical media appears as a read-only source. Alt+O chooses the first applicable
source, matching Windows's independent optical-save command. Blank media of
4096 bytes or less is not offered. Saving an optical source as ISO copies the
exact 2048-byte device sectors; it does not rebuild its filesystem. The existing
ISO export choice for a USB filesystem continues to export files into a new ISO.

Capture acquires one read-only descriptor through UDisks, leaves source mounts
alone, and checks source identity and capacity. It verifies the staged image and
rereads the source before closing the descriptor and publishing the destination.
Read errors, cancellation and verification failures leave the existing image
intact. No source write or source mount is requested.

### Reset and ESP toggling

The reset command opens the selected USB device's actual usbfs node and checks
its identity. A device with multiple disks or a non-storage USB function is
refused: resetting it would also reset another disk or function. This is the
native reason for that difference from Windows's generic port-cycle command.

ESP toggling supports GPT and DOS/MBR tables, including logical partitions.
GPT changes the ESP type to Microsoft Basic Data and restores it to ESP. MBR
uses the original FAT type detection and the ESP type 0xef. Rufus preserves the
original `ToggleEsp01` through `ToggleEsp08` restoration keys. GPT records the
partition GUID; MBR records the original synthetic disk-signature/offset GUID.
The GUI must persist and sync a restoration record before the helper changes a
table. Restoration removes its record after successful helper exit.

The helper rechecks the retained partition-table bytes after the record is
saved, uses libfdisk to inspect the layout, validates both GPT copies, changes only
the type and required checksums in the original bytes, syncs, and asks the kernel
to reread the table. Linux can refresh this metadata directly; Alt+P therefore uses that
reread rather than cycling the USB port as Windows does to refresh its volume
access. Unrelated partitions and boot code are preserved.

### Locking override

Alt+, is a real opt-out of Rufus's `O_EXCL` claim, including reacquisition after
formatter handoff. Identity, system/application/source protection, unmount,
sync and cleanup checks remain active. Kernel partition-table rereads and
filesystem providers may still reject another opener independently of Rufus's
claim. The option is session-only and starts enabled, like Windows's `lock_drive`.

## Original exclusion settings

Rufus reads the original eight `IgnoreUsbNN` and eight `IgnoreDiskNN` keys from
its normal or portable settings, including imported Windows INI files.

```ini
IgnoreUsb01=0x095116ab
IgnoreDisk01={01234567-89ab-cdef-0123-456789abcdef}
```

USB values are unsigned 32-bit decimal or hexadecimal integers: VID occupies the
high 16 bits and PID the low 16 bits. Zero/empty disables a slot. Disk values are
decorated GPT disk GUIDs. Malformed entries are diagnosed and ignored. Settings
reset removes these keys too.

Exclusions apply before listing, alongside the existing system/application and
advanced-class filters. Discovery can use udev's device-number keyed GPT UUID
when raw read permission is absent. The helper checks fresh on-device GPT
headers and USB facts before any write; it does not trust a cached UUID for a
write decision. Invalid or inconsistent GPT headers cannot bypass a configured
disk exclusion. Settings are reread at discovery refresh. The accepted settings are frozen with
the job request.

## Virtual devices and backing storage

The existing virtual-device option also covers native loop and connected NBD
disks, including UDisks objects without a Drive object. A file-backed loop pins
its backing inode, offset, size limit and kernel disk sequence. NBD discovery
pins the connected kernel PID and process start identity. The authorized job
must resolve the local NBD server's backing files. An opaque socket-only server
is refused instead of claiming that its backing files are protected.

The normal writer and helper handle these classes. A display name alone never
establishes eligibility. A capture destination cannot replace a loop/NBD backing
inode, and a retained image cannot be the selected virtual device's backing file.

Source and caller-home protection share the backing resolver used for the
running application. It follows partition parents, stacked block-device slaves,
file-backed loops, Btrfs member devices, positively resolved overlay paths and
absolute FUSE backing paths. Kernel NFS/CIFS mounts require a numeric server address that is neither loopback
nor a local interface address; memory filesystems are known memory cases. A
local or unresolved network export is refused. Unknown anonymous or opaque backing remains a refusal.
Every retained parent/container/supplementary source is protected.

Automatic unmount now includes a single-member Btrfs filesystem with one eligible
mount. Multiple members, aliases and nested mounts remain a refusal because
unmounting or erasing them would affect storage beyond the selected volume.

## Relocated VHD/VHDX parents

Relative chains and immutable VHDX log replay retain their existing behavior.
For a Windows drive, volume-GUID, UNC or relocated parent, create a UTF-8 file
beside the child named `child.vhd.parents` or `child.vhdx.parents`. Each record is
an original locator, a literal TAB, and an absolute Linux path, followed by a
newline. An explicit per-child parent selection can use the reserved locator
`@parent:` so a Windows path need not be transcribed:

```text
@parent:<TAB>/home/marius/images/parent.vhdx
```

Replace `<TAB>` with one actual tab. A map may instead name the exact stored
Windows locator on the left. Maps are bounded to 64 KiB and 32 records, must have
the same owner as the image, and cannot be symlinks. The reader retains and
revalidates maps and all parents before authorization. Parent identifiers,
geometry and VHD timestamps must still match. Mapping a different file does not
bypass those checks. Each child in a multi-level chain can have its own map.
The original image and its locators are never rewritten.

VHD handles relative/absolute Unicode locators and explicit per-child relocation.
VHDX evaluates relative, volume and absolute Win32 locators in their prescribed
order and accepts either valid merge-time parent linkage identifier. The
[Microsoft locator specification](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-vhdx/b6332a98-624d-46b8-bd0e-b77b573662f9)
contains contradictory wording about `parent_linkage2`; its
[Windows product-behavior note 12](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-vhdx/70c6c568-e9cd-464f-ab22-0d2a3c3b1e63)
explains the two identifiers used while merging. Native tests cover matching
either identifier and rejecting a mismatch. This is specification-based native
qualification; it does not claim execution of the Windows desktop API.
