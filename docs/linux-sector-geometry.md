# Sector and filesystem geometry — CP76 / Preview45

B5 covers I12, I13 and A02. The Linux GUI uses the selected device's logical and
physical sector sizes when choosing filesystems and clusters. Discovery includes
both facts in the selection identity. The accepted request freezes them; a changed
snapshot or retained descriptor fails before writes. Raw image writing retains its
existing separate sector policy.

## Native formats and layouts

FAT16, FAT32, NTFS, exFAT, ext2/ext3 and random-access UDF accept logical sectors
of 512, 1024, 2048 and 4096 bytes. Physical sectors are powers of two from the
logical size through 65536 bytes. A 512e device uses 512-byte filesystem sectors;
its 4096-byte physical sectors are a separate checked fact. Missing or malformed
geometry does not grant format eligibility.

MBR, GPT and SFD planners, providers and readback use the actual logical sector.
Kernel sysfs partition offsets and lengths remain in 512-byte units, as Linux
requires. MBR capacity is bounded by 2^32 logical sectors: 2 TiB at 512 bytes and
16 TiB at 4096 bytes. Each formatter also applies its own address and cluster
limits. GPT retains Windows Rufus's 34/33 logical-sector usable-range reservation,
including on 4Kn; descriptor table contents and both copies are checked. Large
boundary fixtures are sparse and actual transfers are bounded.

FAT32 now includes the original selectable 64 KiB clusters. FAT readback on
non-512 geometry checks bounded native metadata without changing shared libfat's
process-global geometry. NTFS checks the complete logical primary/backup boot
sectors, and exFAT checks both complete boot regions and filesystem checksum.
The qualified kernel NTFS driver fails native 4Kn MFT/mirror writes despite
valid 4096-byte MFT/index records from mkntfs. Non-512 NTFS file creation selects
ntfs-3g during capability preflight, before partitioning. Ordinary 512/512e uses
the preferred kernel provider where eligible. An I/O or corruption error stops
the job; it never triggers a provider retry. The combined guest checks successful
userspace copying, clean unmount and independent kernel readback on 4Kn.

Small ext volumes use blocks at least as large as their logical sector, retaining
the original inode profile. This makes small 4Kn ext volumes mountable by Linux.

Original Windows Rufus warns before trying non-512 bootable media
(`src/rufus.c`, start), and its SBR writer retains an explicit 4K TODO
(`src/format.c`, WriteSBR). The native original BIOS, DOS, Syslinux and SBR records,
and the bundled UEFI:NTFS FAT image, retain their 512-byte-sector contract. These
choices fail before partitioning on other logical sectors. Native UEFI FAT media
and non-bootable formats can use 4Kn. Valid formatting alone does not establish
execution of an original BIOS payload on 4Kn.

## UDF blocks and capacity

UDF logical block size is represented separately from device logical/physical
sector size. The selected random-access mkudffs provider requires UDF blocks to
match device logical sectors, so its default chooses that size; an explicit
mismatch fails before writes. Physical 4K sectors on a 512e device do not enlarge
UDF logical blocks. See the [provider manual](https://github.com/pali/udftools/blob/master/doc/mkudffs.8).

The formatter supports at most UINT32_MAX blocks and otherwise truncates the
count. The adapter refuses excess capacity before writing. Exact maxima are:

| Logical UDF block | Native maximum |
| --- | --- |
| 512 bytes | 2 TiB minus 512 bytes |
| 1024 bytes | 4 TiB minus 1024 bytes |
| 2048 bytes | 8 TiB minus 2048 bytes |
| 4096 bytes | 16 TiB minus 4096 bytes |

Microsoft documents a [2^32-block UDF limit](https://learn.microsoft.com/en-us/windows/win32/fileio/filesystem-functionality-comparison).
Original Rufus passes a default allocation size to FormatEx/VDS. Its 256 TiB UI
cap is not an executable formatter guarantee. The Microsoft limit is a documented
Windows bound; actual Windows formatting at that boundary has not been run.
The native provider is explicitly bounded one block below that envelope.

Recognition descriptors retain their 2048-byte format. Anchor/descriptor offsets,
CRC lengths and volume extents derive from UDF block size. Metadata reads use
at most 4096-byte blocks and 64 KiB descriptor sequences. Volumes below 3072
blocks use the provider's two-anchor layout; larger volumes require all three.
The minimum is max(1 MiB, 258 blocks). UDF 2.50/2.60 metadata partitions remain B6.

## FAT32 and Windows backend controls

Original Alt+L forces the large-FAT32 provider and its display name; it does not
change the original cluster list. It also disables full formatting, even below
32 GiB. The native single FAT32 provider retains the original cluster eligibility,
including 64 KiB, and offers ordinary full formatting through 32 GiB; larger FAT32
is quick-only. It does not expose a control that only selects a Windows API.

Original Alt+V selects VDS instead of FMIFS for applicable formats. It does not
change native FAT32 cluster output; the large-FAT32 dispatch precedes VDS.
Linux uses an explicit provider with checked geometry, quick/full behavior,
volume label, serial, sync and readback. Metadata placement can differ between
providers. Native full formatting remains useful below 32 GiB, so forcing a
second provider solely to disable that valid operation would not provide a
coherent additional function. Windows ReFS availability also depends on VDS;
that native provider remains B7 and is not enabled by a backend toggle.

## Qualification

The delivery receipt records grouped normal/sanitizer component checks, one
combined native guest route per build, and one full nine-suite desktop gate.
The guest uses actual virtual block devices at 512/512, 512/4096 and 4096/4096,
with all seven formatters, MBR/GPT/SFD, independent metadata and kernel readback,
stale geometry refusal, sync/cleanup and joined READY. A representative native
FAT32 EFI handoff checks the shared creation path. Physical media and original
Windows execution retain their existing qualification limits.

Grouped normal/sanitizer components qualify 13 unique suites and 778 substantive cases, no skips or outstanding failures. Initial checks found three obsolete/fault-interaction failures; focused repairs preserve the initial receipts and pass. One combined native route per build creates and independently reads back 21 ordinary Qt/helper jobs: seven formats at 512/512, 512/4096 and 4096/4096 across MBR/GPT/SFD, six stale-geometry refusals before authorization, one FAT32 full format, exact payloads where originally eligible and clean ext roots/superblocks, one authorization per job and joined READY. A representative native FAT32 EFI handoff passes. Sparse component fixtures cover capacity/cluster/provider boundaries including the actual maximum native 4-KiB UDF volume. The single full nine-suite Qt run plus a focused normal/sanitizer native-ioctl fixture and cluster-choice repairs qualifies 2,215 substantive cases with 2 existing optional skips and zero outstanding failures. The full run exposed an obsolete ioctl fixture that omitted BLKPBSZGET and one cluster-choice assertion expecting four options before 64 KiB was restored; all 53 cases in the affected device suite and the single cluster row pass after repair. Prior unchanged writer faults and installed-Windows/customization evidence are reused.

Compact evidence: `../../.rufus-test-artifacts/cp76/checks.json`.
