# Native UDF and ext data formatting

CP76 extends these formats to actual logical/physical sector geometry. See the
[sector and capacity guide](linux-sector-geometry.md) for 512/512e/4Kn, UDF block
sizes, provider maxima and the combined B5 qualification.

CP60 groups UDF data formatting and the missing non-bootable ext2/ext3 layouts.
Choose **Non bootable**, then MBR, GPT or **Super Floppy Disk**. Expand advanced
format options to see ext2/ext3, matching the original Windows controls. These
choices use the existing confirmation, one authorized helper and joined READY.

| Filesystem | Layouts | Native provider | Boundary |
| --- | --- | --- | --- |
| UDF | MBR, GPT, SFD | Optional udftools `mkudffs` | 512/1024/2048/4096-byte logical sectors; revisions 1.02, 1.50, 2.00 and 2.01; at most UINT32_MAX volume blocks. |
| ext2/ext3 | MBR, GPT, SFD | Required e2fsprogs `mke2fs` | Original 256-MiB minimum device size; existing native geometry and label limits. |

The cluster chooser shows **Default**, as Windows does. UDF labels retain up to
32 UTF-16 units, including Unicode. `UseUdfVersion=0` or an absent preference
selects 2.01; an explicit supported revision is frozen with the job. Unsupported
revisions stop before authorization/writes. Existing standalone GRUB2 ext2/ext3
and ext2/3/4 persistence retain their original routes. The checked Windows UI has
no selectable standalone ext4 size row; this batch preserves that behavior.

## Provider and formatting behavior

UDF uses a fixed configured executable (`RUFUS_MKUDFFS`, normally
`/usr/bin/mkudffs`), not a shell or a PATH lookup inside the authorized helper.
A private 1-MiB dry-run file checks the selected revision, UTF-8 label and HD
options before partitioning. A missing or incompatible provider leaves the
selected device untouched. Packaging lists udftools as optional; the formatter
is supplied by the distribution, not bundled into the application.

Quick UDF formatting creates filesystem metadata. Full formatting first clears
and reads back the entire volume using the existing bounded zeroing writer.
Both modes use the retained native target descriptor, sync, and check the UDF
recognition sequence, the provider's two or three anchors, main/reserve volume descriptors,
closed integrity record, label/revision and root directory metadata. These
checks verify the newly created layout; they are not a general UDF repair tool.
Ext modes keep CP47's native initialization rules: full ext3 initializes its
journal, and ext2 full/quick do not imply a free-space wipe.

UDF supports existing extended label/icon files and Ctrl+SELECT supplementary
ZIP contents. The helper mounts it privately with UTF-8 names and makes the
appearance files readable after root formatting. Ext data formats omit these
controls, following Windows eligibility. UDF is also admitted by the existing
automatic source-unmount and read-only ISO-export mount paths. Formatting is
data-only: no OS or bootloader is installed by these choices.

## Deferred UDF 2.50/2.60 gap — user decision 2026-10-24

**B6 / I14 is deferred from the useful-release work.** Existing UDF 1.02,
1.50, 2.00 and 2.01 formatting remains supported on MBR/GPT/SFD, including
quick/full formatting, labels/icons and supplementary ZIP contents. The default
remains 2.01. An explicit unsupported revision is refused before authorization
or writes; another revision is never silently substituted. B5's qualified
sector geometry and provider capacity limits remain unchanged.

UDF 2.50/2.60 metadata-partition creation is unimplemented, not delivered or
declared equivalent to the older revisions. The user deferred it after review
found no demonstrated ordinary USB workflow requiring these newer revisions.
Full Windows parity remains incomplete, while release preparation can proceed.

The provider gap is substantial: udftools 2.3 cannot create HD metadata
partitions, and the standard [Linux UDF driver](https://raw.githubusercontent.com/torvalds/linux/master/fs/udf/super.c)
refuses writable metadata-partition mounts. A formatter alone would therefore
not provide the existing label/icon and supplementary-file operations. The
[NetBSD formatter core](https://raw.githubusercontent.com/NetBSD/src/trunk/sbin/newfs_udf/udf_core.c)
provides a possible 2.50 foundation but explicitly rejects 2.60; no complete
provider was qualified. Actual Windows 2.60 formatting behavior on random-access
USB media also remains unverified; the saved UseUdfVersion setting alone does
not establish that support. This was a source/specification audit, with no new
formatting tests or target writes.

Resuming B6 would require a provider capable of creating and populating genuine
metadata-partition media, independent allocation/descriptor/file readback, and
the original eligible completion/cancellation behavior. See the official
[udftools release](https://github.com/pali/udftools/releases/tag/2.3),
[mkudffs options](https://github.com/pali/udftools/blob/master/doc/mkudffs.8)
and [Arch provider package](https://archlinux.org/packages/extra/x86_64/udftools/).

## ReFS formatting excluded from the first release

The user also removed ReFS formatting (I15) from first-release requirements on
2026-10-24. It remains unimplemented and disclosed as a full-parity gap. The inspected
[native refsprogs project](https://github.com/unsound/refsprogs) supplies access
utilities rather than a formatter. The user subsequently also deferred Dev Drive detection. CP77 qualifies the
supported-UDF saved choice and optional unavailable diagnostic; actual UDF
indexing metadata remains unimplemented. See the [metadata policy](linux-filesystem-metadata-policy.md). Indexing hints on the deferred newer-UDF/ReFS providers
follow those providers. Full native translations also remain in release scope.
Extended-label discovery and the qualified DOS attribute routes are delivered;
see the [current register](linux-parity-register.json) for remaining work.

## Batched qualification

The focused group checks all nine filesystem/layout request combinations,
original eligibility, strict protocol refusal and real file formatting with all
four UDF revisions, Unicode/empty labels and full/quick semantics. Normal and
ASan/UBSan/LSan builds run the same group.

One combined Linux guest per build drives four ordinary Qt/helper jobs on an
owned simulated USB: UDF/GPT/quick with icons and a Unicode ZIP overlay,
UDF/SFD/full, ext2/SFD/quick and ext3/MBR/full. Independent kernel readback checks
files/metadata and reacquires the released claim. Each job checks one
authorization, held completion until helper exit and joined READY. These are
non-bootable data formats, so earlier boot/writer, polkit and fault evidence is
reused. The batch has one full Qt regression gate. No host block devices,
Windows execution or installation tests are involved.

Compact logs, commands, provider provenance and the final delivery receipt are
under `../.rufus-test-artifacts/cp60/`. This batch does not complete the full
Windows parity goal.


## CP70 — NTFS compression and indexing (Preview 39)

The [NTFS options batch](linux-ntfs-options.md) groups session Alt+N compression,
saved Alt+Q indexing, frozen NTFS-only options, native formatter defaults and
boot-file compression exceptions. Larger clusters continue uncompressed with
a diagnostic. Compressed file creation preselects the qualified NTFS-3G provider
before writes; ordinary NTFS creation keeps the kernel provider. No write or
corruption error switches drivers. The guide records native directory-default
restoration, provider evidence and remaining UDF/ReFS indexing-hint work.
