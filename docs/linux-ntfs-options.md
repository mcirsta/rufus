# NTFS compression and file indexing

CP70 / Preview 39 adds the original **Alt+N** compression switch and **Alt+Q**
file-indexing preference to native NTFS creation. Compression starts disabled
each session. Indexing starts disabled and is saved as `EnableFileIndexing`.
The accepted values are frozen with the complete job before authorization.
They apply to NTFS formatting and copied media; DD writes remain unchanged.

## Formatting and providers

The native formatter uses `mkntfs -C` for compression and omits its usual `-I`
when indexing is enabled. Indexing here is the NTFS root's Windows
`NOT_CONTENT_INDEXED` hint; it does not start an indexer on Linux. The existing
quick/full format, label, geometry, synchronization and backup checks remain.

NTFS compression requires clusters no larger than 4096 bytes. With larger
clusters the requested format continues uncompressed and logs the reason,
matching Windows's optional volume-compression attempt. Ordinary NTFS jobs keep
the preferred kernel provider. Effective compressed file creation selects
**NTFS-3G** before partitioning. Install the distribution's `ntfs-3g` package
when using compression with copied files, boot files or extended-label files.
A missing required provider refuses before target writes. A write, corruption,
sync or unmount error never triggers a retry through another driver.

The guarded 7.2.11 kernel guest exposed missing creation inheritance and an
unreadable compression-unit record after explicitly setting a new file's flag.
The existing foreground NTFS-3G provider supplies the required creation semantics
and remains owned by the same authorized helper until unmount and exit. This is
a deliberate native capability choice. The official
[kernel NTFS implementation](https://github.com/torvalds/linux/tree/master/fs/ntfs)
and [NTFS-3G compression documentation](https://github.com/tuxera/ntfs-3g/wiki/Using-Transparently-Compressed-Files)
describe the separate native interfaces; the CP70 receipt records actual results.

## Boot files

Windows `src/iso.c` runs `compact /u bootmgr* efi/boot/*.efi` for an eligible
Windows image after copying. Linux produces those files uncompressed from the
start. NTFS-3G permits changing a directory's compression default, but its
attribute setter does not change compression on an individual file. The helper
temporarily disables the parent default, creates the file and restores the
default before copying its bytes. It is the only writer in its private,
exclusively claimed volume. Both success and failed creation attempt restoration.
Failure to establish or restore required boot metadata stops the incomplete job.

The same operation protects generated Windows boot files and the native
Syslinux/GRUB4DOS loader paths, whose existing sector mapping and boot bytes must
remain usable. Other ISO files inherit compression from the NTFS-3G directory.
No in-place rewrite of populated compressed boot files is required.

## Qualification and remaining work

Focused tests cover real shortcut activation, saved/session behavior, strict
NTFS-only protocol admission, all compression/indexing combinations and both
4096/8192-byte clusters. Independent `ntfsinfo` inspection checks root metadata
in actual formatted files. Existing native format/copy/loader fault evidence is
reused where unchanged.

One combined normal/sanitizer disposable guest route drives ordinary Qt jobs
for compressed data formatting, the larger-cluster uncompressed fallback and a
known Windows-metadata ISO with a native EFI fixture. Independent kernel
readback checks root hints, complete logical payload hashes, reduced physical
allocation and uncompressed boot-file flags. One representative UEFI:NTFS/native
EFI boot and a missing-provider prewrite refusal complete the batch. No Windows
application or installation runs; no host device is mounted or written.

Windows also attempts the indexing hint on UDF and ReFS. CP77 preserves the supported-UDF saved
choice and reports the unavailable optional attribute; actual UDF indexing
metadata is not written. ReFS formatting/indexing follows its deferred provider.
See the [native metadata policy](linux-filesystem-metadata-policy.md). This batch
closes the NTFS controls.
Other filesystem/provider/architecture and distribution qualification remains in
the [complete Windows parity backlog](linux-feature-parity.md).

## Native sector geometry

CP76 retains the preferred kernel driver for eligible 512/512e creation. The
qualified kernel NTFS write path fails MFT/mirror writes on non-512 logical
sectors. Those file-creation jobs select ntfs-3g before partitioning; a later
I/O error still fails the job. See the [geometry guide](linux-sector-geometry.md).
