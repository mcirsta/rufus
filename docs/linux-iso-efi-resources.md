# ISO extraction, EFI and boot resources — B3 / CP74

## Creation behavior

Ordinary ISO creation accepts source-offered `bootia32.efi`, `bootx64.efi`,
`bootarm.efi`, `bootaa64.efi`, `bootia64.efi`, `bootriscv64.efi`,
`bootloongarch64.efi` and `bootebc.efi`. Every offered fallback must have matching
PE machine, PE32/PE32+ format and EFI application subsystem. ARM Thumb mixed
machine 0x01c2 and the original ARM spelling are supported. Selection follows
the source image. Windows image-1 EFI repair remains x64. IA64/EBC entries are
preserved; the Windows resource set contains no runtime validator for those two
architectures. [UEFI machine definitions](https://uefi.org/specs/UEFI/2.9_A/02_Overview.html)
and the checked-out Windows `efi_archname` table define the names.

Embedded FAT EFI content is copied when the original scanner selects embedded
EFI or its broken small x64 fallback repair. It uses the retained ISO/UDF view,
including El Torito virtual files, and the original first active MBR FAT partition
rule. Ordinary source files take precedence; only the identified broken fallback
is replaced. Embedded applications participate in preflight, security analysis,
space/copy progress and checksum updates. Mint's dangling fallback is omitted
from banner parsing only when its actual repair image is readable; other source
content failures retain their original refusal.

Rock Ridge links resolve only inside the source image. NTFS gets relative Windows
`IO_REPARSE_TAG_SYMLINK` file/directory records. FAT keeps original literal-target
and nonzero-data skip behavior, duplicates `/firmware` file targets and materializes
a `syslinux` to `isolinux` directory alias with `syslnx32.cfg`/`syslnx64.cfg`
redirects. Source cycles, escaping paths and conflicting output types fail.
Supported UDF nonregular entries are read as literal file data, matching the
original entry-as-file rule. POSIX nonregular records produce ordinary output;
they cannot create host devices, FIFOs or sockets. Missing safe parent directories
are recovered for incorrectly mastered nested names. Slax's `/slax/boot/EFI`
moves to `/EFI` when root EFI is absent. Queued relative links and checksum paths
follow that relocation.

## Syslinux resources

The source version selects the original contract: families 3/4 use the bundled
4.07 loader pair; 5/6 use matching `ldlinux.sys`/`ldlinux.bss` and retain the source
`ldlinux.c32`. The full version/suffix cache is tried first. Signed downloads use
the original Rufus resource service; a missing full-suffix payload can fall back
to the base version. A missing same-numeric 6.04 download can use the embedded
6.04 pair. Signature rejection and cancellation stop selection. Declining a
required pair stops creation. For old families, declining the optional signed
`menu.c32`/`vesamenu.c32` replacements preserves the source modules. Replacement
uses the original inclusive 53,500/148,000 byte thresholds.

Selection, signature checking and source identity checks run before confirmation
and authorization. A bounded owned resource bundle is retained for the one
helper job, validated again against the freshly analyzed source and passed to
the existing native patcher/providers. No elevated network/cache lookup occurs.
Native providers retain private aligned templates and exact boot/file readback.
Syslinux v4's native endian accessors use the same byte-safe implementation on
x86 and aarch64; Windows sources and dependency source trees are unchanged.

## Native differences and limits

- New kernel `ntfs` writes LX symlinks. Windows reparse creation therefore selects
  the existing `lowntfs-3g` provider before partitioning. Its availability is checked
  before authorization and writes. A write or corruption error cannot switch
  drivers. Ordinary NTFS creation retains its existing provider selection.
- Output paths stay relative to private descriptors, with no host symlink
  traversal. Malformed source trees and unsafe/ambiguous links fail the job.
  Embedded trees are bounded to 32 levels, 65,536 entries, 64 MiB per file and
  512 MiB total. Individual boot-resource files are bounded to 512 KiB and their
  transport bundle to less than 1 MiB. These native parsing limits can refuse
  oversized or malformed input that Windows might attempt.
- Slax move, config backup, reparse readback, synchronization and cleanup errors
  fail creation. READY still requires successful helper and provider exit.
- Issuer metadata prefers a unique verified embedded immediate issuer, then a
  unique issuer from OpenSSL's configured local system certificate bundle.
  Missing, unavailable or ambiguous issuers retain the signer. The cache is
  bounded (16 MiB / 4,096 certificates); caller `SSL_CERT_FILE` overrides and AIA
  network fetching are excluded. This classifies issuer metadata and existing
  revocation policy; it does not authenticate the publisher.
- Qualification uses generated media, original signed Syslinux 5.10/6.03 resources
  and the existing pinned native providers. It does not certify every distribution,
  every firmware, physical USB hardware or Windows API execution. No additional
  installed-Windows/customization matrix was added.

## Batched evidence

The compact receipt is `../../.rufus-test-artifacts/cp74/checks.json`, with source
hashes, scoped patch, logs and package checks. The gate groups EFI architecture,
embedded FAT, links/nonregular entries, mastering recovery, Slax/checksums,
signed resources/source identity/cancellation, COM32 thresholds, issuer ambiguity
and portable accessors. Normal and ASan/UBSan/LSan runs share that corpus.

One combined guest route per build creates and independently reads back 5.10 FAT32,
6.03 NTFS, old-family 3 FAT32 and 4 NTFS media. It checks original EFI bytes,
literal/Windows file and directory links, raw NTFS reparse metadata, replacement
modules/checksums, one authorization, joined READY and released claims. One
representative shared-writer BIOS handoff reaches the source Syslinux config;
one EFI handoff uses the embedded application through original UEFI:NTFS.
The ntfsprogs-plus metadata reader rejects the original Syslinux VBR jump;
its metadata-only readback copy normalizes those three bytes as in CP20k. Actual
USB payload/readback and firmware boots retain the original VBR.
Aarch64 v4 cross-build, `--version` and unaligned accessor execution run under
user emulation; physical aarch64 media writes remain unqualified. The batch ends
with one full nine-suite Qt gate and inspected Arch/generic Preview43 packages.
Existing unchanged writer/helper fault and representative boot evidence is reused.
Owned guest disks/trees are removed after exporting compact logs and checksums.
