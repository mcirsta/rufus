# Supplementary ZIP archives — CP57

Hold **Ctrl** while pressing **SELECT** to attach a ZIP containing additional
files. Select the boot image first: choosing a new image clears the attachment.
Opening Ctrl+SELECT again clears the previous attachment, including on Cancel,
matching Windows. The SELECT tooltip and creation confirmation show the ZIP.

After normal formatting or image copying, the same authorized helper extracts
the archive into the main filesystem. It can replace files copied from the ISO
or generated earlier, including label/icon files. Central-directory order wins
when names repeat. The GUI stays unprivileged. Sync, private-mount cleanup and
helper/launcher exit precede successful full READY.

## Windows reference and eligibility

`src/rufus.c:IDC_SELECT` assigns `archive_path` when Ctrl is held and clears it
on a subsequent normal image selection. `src/format.c` calls `ExtractZip` after
the creation/finalization work, when `fs_type < FS_EXT2`. Its raw/DD branch
returns earlier. `src/stdio.c:ExtractZip` delegates to Bled's directory reader.

Linux keeps the same eligibility rule. Current native creation supports
FAT16/FAT32, NTFS, exFAT and CP60 UDF, including supported standalone/non-bootable
and SFD routes. DD and ext jobs omit the attachment. ReFS and later UDF revisions
remain in the filesystem backlog; a ZIP does not enable a missing provider.
Legacy WinPE's final boot-record masquerade is deferred until the ZIP mount
has been cleaned up, preserving Linux's existing partition-refresh requirement.

## Native source and output rules

- Freeze the archive's absolute path, device/inode, size and modification/change
  timestamps before creation. The helper retains a caller-owned readable file.
- Validate every entry's path, size and CRC before target access. Protect both
  the main image and the archive's backing storage before any target unmount or
  write. Changed sources are refused; archive bytes are never modified.
- Share the retained ZIP/ZIP64 parser and stored, deflate, BZIP2, LZMA and XZ
  decoders with ZIP disk images. Their first-entry DD selection stays unchanged.
- Resolve output relative to the private mounted directory; reject path escape,
  symlinks, hardlinked output files, special nodes and nested filesystem traversal.
  Preserve UTF-8 names; legacy names use CP437. Ordinary files replace earlier
  files, and existing directories are reused. Unix link metadata is copied as
  ordinary payload bytes, matching the Windows reader's file/directory handling.
- Check free space per member, allowing replacement to reclaim its old allocated
  blocks. A failed current copy is removed; earlier completed entries remain.
  There is no rollback of an overwritten file or of the prepared device.

Windows can log a warning for an extraction failure. Linux marks a requested
supplement failure as FAILED: a successful READY must mean the requested files
were copied and synced. This is the same completion rule used by native image
extraction. Read, CRC, mutation, cancellation, write, sync and cleanup failures
cannot become successful READY. Empty archives complete without adding files.

The archive walker is bounded to 262144 entries, 64 MiB of aggregate name bytes,
128 directory components and 4095 UTF-8 path bytes. Per-file size follows the
target capacity and FAT's 4-GiB-minus-one limit; aggregate counters use checked
64-bit bounds. Encryption, split volumes and unknown methods remain unsupported
by the existing native provider. Filename rules of the target filesystem apply.
Parsing references the [PKWARE ZIP specification](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT).

## Qualification

One grouped native normal/sanitizer corpus covers all five codecs, multi-entry
ZIP64, streaming descriptors, more than 65535 entries, repeated names, Unicode,
path/source protection and partial-I/O/cancellation/sync failures. The existing
ZIP disk-image regression verifies unchanged DD behavior. Focused Qt tests cover
Ctrl+SELECT, clearing/reselection and strict request/result/progress fields.

One combined guest per build covers a bad-CRC prewrite refusal, ordinary Qt
formatting with an archive, a source-on-target refusal and bootable Syslinux ISO
creation with a final ZIP overlay. Independent filesystem readback and BIOS boot
qualify the resulting media. Existing helper acquisition/cleanup fault evidence
is reused, and the nine-suite Qt gate runs once for this batch. It exposed four
bootloader-download progress failures: the new creation-completion cap also
capped downloads. Download progress keeps its existing full range; creation
stays below full until completion. The affected download rows and the existing
READY-after-helper-exit checks are rerun together after that correction. Final
gate and package results are recorded in `.rufus-test-artifacts/cp57/checks.json`.
