# Windows-image metadata: shared code and native WIM access

Updated: 2026-09-14. This is the CP3g boundary, based on the checked-out Rufus
sources. It concerns read-only image metadata; device operations, installation
choices, Windows customization and Secure Boot remain separate.

## Source map and decisions

| Operation | Existing Rufus implementation | Linux/shared treatment |
| --- | --- | --- |
| Windows/WinPE file presence | `image_file.c`, shared by both tree walkers | Retain the existing names and WinPE masks. CP3g1 also records the original-case SIF paths. |
| WinPE `/minint` decision | `ExtractISO` reads `txtsetup.sif`, uses `get_token_data_buffer`, lowercases the value and searches for `/minint` | CP3g1 shares this scan in `winpe_scan.c`. It uses the SIF-relevant line grammar from `parser.c`, with no wide-character conversion or GUI dependencies. |
| WIM archive-format version | `vhd.c:GetWimVersion` opens a WIM and returns `wimlib_get_wim_info().wim_version` | CP3g2 shares the handle query in `ReadWimArchiveInfo`; Windows retains its existing UTF-8/container path opener. |
| Windows release/build | `wue.c:PopulateWindowsVersion` reads the first install image's XML, then `PopulateWindowsVersionFromXml` reads MAJOR/MINOR/BUILD/SPBUILD | CP3g2 shares `NormalizeWindowsVersion`. `ReadWimMetadata` uses wimlib properties and explicit validity masks; the existing Windows XML/dialog callers retain their conversion behavior and call the shared normalization. |
| Edition/index choice | `wue.c:GetEditions` combines WIM XML enumeration with a selection dialog | Separate plain edition/index data from the frontend; the dialog stays in Widgets/Win32. This is later than automatic metadata inspection. |
| WIM-contained EFI file extraction and architecture | `vhd.c:WimExtractFile`, `rufus.c:ImageScanThread`, `FindArch` | CP3g4a implements bounded native single-file extraction and shares PE architecture recognition. SELECT integration remains separate. Presence is not signature or boot validation. |

The existing Windows version rules choose the first installation path and first
WIM image for automatic reporting. The normalization maps Windows 6.1/6.2/6.3/6.4
to 7/8/8.1/10 and treats a Windows 10 build above 20000 as 11. These are Rufus's
current reporting rules, not a new claim about Windows product identification.
Keep the archive-format field `wininst_version` separate from `win_version`.

## Why native wimlib access needs its own checkpoint

Rufus's bundled wimlib has a custom `WITH_LIBCDIO` path: `wim.c:open_iso_wim_file`
accepts `image.iso|internal/path`, and `file_io.c` implements ISO/UDF reads.
The public native `wimlib_open_wim` API opens a WIM filename; this custom
container-path behavior must not be assumed when substituting a system library.
The bundled source/build is oriented toward Windows: its source list includes
Windows capture/apply implementations, while the corresponding Unix source
files are absent from this checkout. Linking it natively is more than replacing
one Windows API call. The host had no native wimlib at CP3g1. CP3g2 now uses a
checksum-verified native distro library extracted under `/tmp`, without
installing system packages.

Proceed in two bounded steps:

1. **CP3g2: native WIM metadata on ordinary files. Implemented.** Native wimlib
   supplies archive information and XML properties; shared C code normalizes
   version values. Generated WIMs cover image counts/properties, missing/unknown
   values, failures and cancellation. No additional WIM container/XML parser.
2. **CP3g3: WIM inside UDF/ISO9660. Metadata implemented.** Upstream wimlib
   1.14.5 with a small reader extension uses the selected filesystem view,
   bounded reads and cancellation. SELECT reports installation metadata without
   copying whole WIMs into RAM or temporary files. The source comparison and
   transport tests below record this choice; extraction remains a separate step.

## CP3g1: implemented WinPE contract

`ScanWinPeOptions` preserves the original i386, then AMD64, then minint priority
and the existing `HAS_WINPE` masks. The AMD64 mask deliberately uses the i386
loader markers together with the AMD64 SIF marker; this checkpoint does not
change that recognition policy. Original-case paths from classification replace
hardcoded lowercase lookup when available.

`InspectWinPeOptions` works on caller-owned ANSI/UTF-8 bytes and retains the
first nonempty `OsLoadOptions` value, optional quotes, and the original
case-insensitive `/minint` substring rule. For example `/minint-extra` still
matches. Unquoted semicolon text remains part of the value, as in Rufus's helper.
The line parser also retains its optional angle-bracket delimiters. It is scoped
to SIF lines; the general parser's RTF brace grouping is not carried into this
operation. `parser.c` and its update-data consumers remain unchanged.

Intentional corrections: ordinary text no longer needs a NUL in the file's last
byte; one trailing NUL is accepted and a UTF-8 BOM is skipped. Embedded NULs,
UTF-16, empty files and files above 64 KiB remain unreadable/unsupported. The new
`ReadISOFileToBufferLimited` enforces the requested limit before data allocation
and reading; the existing read API keeps its 1 GiB limit and ownership contract.

`uses_minint` holds the decision; `winpe_options_known` distinguishes checked
absence from unavailable options. Failed/cancelled scans clear both fields.
Successful WinPE-only metadata clears the Windows pending reason. A boot manager
or install WIM still retains that reason for its separate metadata stages.
The shared operation contains no GUI calls, credential requests or media writes.
Windows calls it from the existing scan position; native Windows validation
remains deferred. Qt uses the existing status, label and log controls and keeps
START disabled.

See the [checkpoint record](testing-and-checkpoints.md) for tests and commands.

## CP3g2: ordinary WIM metadata contract

`src/wim_metadata.c` contains the shared archive query, version-property query
and Rufus's unchanged version normalization. It accepts an open `WIMStruct`
and borrows it; the caller owns opening, closing and library initialization.
The fixed ASCII property names and decimal values work with wimlib's native
`char` and Windows `wchar_t` APIs without a callback bridge or platform branches.
The library handles WIM storage and XML decoding.

`RUFUS_WIM_METADATA` separates archive version/count/boot index/part numbers,
raw version fields, normalized Windows version and valid/invalid field masks.
Each missing field has neither bit set; a malformed value has the invalid bit.
Unsigned decimal values must fit `winver_t`'s 16-bit fields and at most 32 text
characters. Signs, whitespace, non-ASCII digits, suffixes and overflow remain
unknown. This strict conversion is used by the new query, not retrofitted into
the existing Windows XML dialog code.

MAJOR, MINOR and BUILD must all be valid before normalization. Missing SPBUILD
can leave a recognizable Windows release/build with an explicitly unknown
revision. The native operation returns PARTIAL until all four fields are valid
and the normalized major/build are nonzero. Successful archive access alone
never means that a Windows version or bootable installation has been verified.
Image indexes are 1-based; callers use image 1 for automatic reporting, even
when another image is marked bootable. An unavailable index fails explicitly.

`linux/platform/wim_file.c` opens the ordinary file read-only, rejects directories
and FIFOs, and passes its held `/proc/self/fd` path to native wimlib. This retains
the selected file across pathname replacement; it does not snapshot its bytes
against concurrent in-place changes. No extraction, mounts, credential requests,
device writes or `wimlib-imagex` process are involved. The host calls
`wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8)` before starting workers and
`wimlib_global_cleanup()` after joining them. The query never resets global
library state; Windows keeps its existing lifecycle.

Cancellation uses the existing direct `IsIsoScanCancelled()` host function
before opening, after opening and after querying. An in-progress wimlib call
cannot yet be interrupted. Error/cancellation discard metadata and release the
handle; failures also retain the library error code. The numeric parser's bound
does not cap all allocation or I/O performed internally by wimlib.

Native CMake targets use the native library's matching header and binary.
Debian/Ubuntu builds require `libwim-dev`; Arch builds require `wimlib`.
`CMAKE_PREFIX_PATH` or `WIMLIB_INCLUDE_DIR`/`WIMLIB_LIBRARY` supports a rootless
installation. The reporting-only build remains independent of wimlib. The local
tests ran against wimlib 1.14.5; the Ubuntu CI jobs still need their first remote
run. Generated normal and solid WIMs report format versions `0x10d00` and `0xe00`
respectively; these values are preserved as library facts, not Windows versions.

**Following CP3g2:** This component is not yet connected to Qt SELECT or WIMs inside
ISO/UDF; those checks stay pending in the existing analysis. Keep the Win32-mapped
Widgets form unchanged. Assess Rufus's libcdio transport, selected filesystem
view, bounded reads and cancellation before adding container access. Secure Boot,
edition selection, WIM extraction/splitting and actual booting remain separate.

## CP3g3a: shared file ranges and the native-library boundary

**Implemented:** `src/iso_file.c` now owns the file-opening, cache and range
reading previously inside `embedded_efi.c`. EFI inspection uses it immediately;
the same operation is the candidate input for WIM transport. The interface has
one concrete `ISO_FILE_READER` and open/read/close functions, without a new
callback registry, GUI dependency or Windows API emulation.

It borrows exactly one selected ISO/UDF handle, owns its UDF file entry and uses
the existing bundled libcdio. Its 32 KiB cache serves arbitrary byte offsets,
including reads crossing cache/block boundaries and backward reads. UDF reads
continue across short/long allocation extents; ISO multi-extents retain libcdio's
existing contiguous-extent requirement. No companion-filesystem fallback occurs.
Empty files are valid even when the ISO generator supplies a non-data extent
sentinel; no physical read is issued for them.

Ranges are checked against the 64-bit file size before I/O, with cancellation
checks on cache hits and between physical calls. Refills request at most sixteen
2048-byte blocks. The final physical block may include on-disc padding; only
logical file bytes reach the caller. Failure invalidates the refill, and callers
discard the requested output if the operation does not return OK. Reads allocate
no additional buffer; the caller owns the result buffer and fixed reader storage.

### Why this needs a source-level wimlib integration

The installed native 1.14.5 header and the
[public wimlib API](https://wimlib.net/apidoc/wimlib_8h.html) expose filename-based
opening. The progress callback reports work; it does not provide random reads.
There is no public arbitrary-reader input hook. Rufus's custom input already
exists in these bundled source locations:

- `wim.c:open_iso_wim_file` selects and opens ISO/UDF files.
- `file_io.c:full_read/full_pread` routes reads, with separate byte/block helpers.
- `wimlib/file_io.h:struct filedes` holds the input backend and current offset.
- `wim.c:begin_read` supplies file size and read-only policy; handle destruction
  closes the selected backend.

That design is useful, but it cannot be enabled by a flag on a distro binary.
It also reopens the container and chooses its filesystem internally, so using
it unchanged would not preserve the view selected by the Linux scan. Sequential
position, short-read reporting and cleanup must follow the shared reader's tested
contract when adapting this boundary. These are integration requirements, not
a new parser for WIM storage or XML.

| Route | Decision for the next step |
| --- | --- |
| Public system wimlib plus a small application adapter | Sufficient for ordinary WIMs, but lacks the reader entry point needed for embedded files. Keep CP3g2 working. |
| Native build retaining Rufus's bundled wimlib fork | Initial prototype candidate. The comparison exposed avoidable Windows portability work; the user chose upstream source instead during CP3g3b. |
| Upstream wimlib source with a small input extension | Selected and qualified in CP3g3b. Reuse the existing input boundary and Rufus's shared ranges, with upstream native build support and parsing. A dedicated fork remains optional for release maintenance. |
| Add a general reader API to upstream wimlib | Possible later contribution if the native prototype demonstrates a small useful interface; not necessary to design a generic framework now. |
| Mount a container or copy the entire install WIM to a temporary file | Adds service/mount lifecycle or image-sized I/O/storage. Not the default for unprivileged metadata inspection. |

The first measurement is at the transport boundary: two native-generated WIMs
of 81,424 and 81,418 bytes were placed in ordinary ISO/UDF containers. The suite
compares 814 ranges (9,451,034 returned bytes across 102 probe runs), including
different valid WIMs at the same path in the two filesystem views. Four scattered
reads within one cached region require one 32 KiB refill. Fragmented UDF files
retain logical byte order. These are byte-transport measurements, not native
wimlib-in-container metadata or boot qualification.

**CP3g3b is now qualified as an opt-in prototype.** The user selected upstream
wimlib source plus a small reader extension. It uses the exact upstream commit
recorded by Rufus for the initial comparison. Five upstream files have 44 added
and 5 removed lines, plus a concrete adapter; parsing remains upstream. The
[prototype report](linux-wimlib-prototype.md) records the dependency decision,
source pin, ownership contract, 132 direct checks and 95 functional probes.
The full 33-test suite passes normally and with ASan/UBSan/leak detection.

**CP3g3c is implemented:** SELECT uses the extended upstream provider and the
scan's retained filesystem handle. `AnalyzeIsoImageWithSource` owns exactly one
selected handle after return; `CloseIsoAnalysisSource` closes it after all content
readers finish. The existing `AnalyzeIsoImage` wrapper still closes on return.
Neither shared operation depends on wimlib or a callback interface.

`linux/platform/windows_image.c` reads the first recorded installation path and
WIM image 1. It stores archive facts separately from normalized Windows fields
and distinguishes unchecked, known, unknown and failed metadata. Unknown/error
results do not select a later candidate. An unreadable WIM keeps the tree result
partial; cancellation clears both reports. A known WIM clears the Windows pending
reason only when any legacy WinPE options are also known. Secure Boot and other
pending reasons are untouched. Boot-manager-only images without an installation
WIM retain their separate Windows check.

The worker initializes wimlib before moving to its thread, cleans up input
ownership before publishing, and carries these results under the existing request
number. The controller discards cancelled/obsolete results and clears Windows
facts when selecting another image. The current status/log display version facts
or the reason they remain unavailable; the Win32-mapped form is unchanged.

The full **34-test suite** passes normally and with ASan/UBSan/leak detection.
There are 22 additional native integration probes and 15 additional Widgets
cases/data rows. The [checkpoint record](testing-and-checkpoints.md#cp3g3c-select-windows-metadata-2026-09-14)
details coverage and reproduction. The system provider is optional comparison
infrastructure; the application uses only extended upstream wimlib. Windows
runtime, WIM extraction, Secure Boot and actual boot qualification remain deferred.

**CP3g3d qualifies upstream release 1.14.5** for this metadata path. The exact
five-file reader patch and concrete adapter are unchanged. The release input
manifest, runtime version and CI archive/checksum now agree; the original
comparison pin is rejected. See the [release qualification](linux-wimlib-prototype.md#cp3g3d-release-qualification)
and [test record](testing-and-checkpoints.md#cp3g3d-wimlib-1145-release-2026-09-14).
WIM-contained EFI inspection is extended in CP3g4a below.

## CP3g4a: bounded single-file extraction and shared PE recognition

`linux/platform/wim_read_file.c` uses the existing public wimlib directory-query
and path-extraction APIs. It queries exactly the selected image and path before
creating output. Directories, reparse points, encrypted files, device entries,
named streams and files larger than **16 MiB** are rejected. Globbing and
recursive extraction are not enabled. Path matching follows the host's wimlib
initialization flags; Linux's default is case-sensitive.

The library extracts one regular file into a `mkdtemp` directory with mode 0700.
Only its basename is written, with ACL/attribute restoration and reparse fixups
disabled. The helper opens the result without following symlinks, verifies its
type and exact declared length, then reads it in 32 KiB chunks into a bounded
caller-owned buffer. Empty files return NULL/0 successfully. The temporary file
and directory are removed on success, error or cancellation; cleanup failures
are reported. No whole-WIM staging, stdout redirection, mounts or root access.

The caller exclusively borrows an open WIM without an existing progress handler.
The helper installs a named native cancellation function for extraction, then
removes it before returning. It also checks the existing host cancellation query
around metadata/extraction and between output reads. Error/cancellation clears
both output values. No exported resources or WIM references escape. The caller
frees the WIM before closing any ISO/UDF reader and filesystem it borrows.
The upstream five-file input patch needs no changes for this operation.

The 16 MiB limit bounds this file's output and buffer. It does not globally bound
wimlib's directory metadata, allocations or solid-resource decompression, and
cancellation cannot interrupt every synchronous library operation. Split/delta
resource resolution and encrypted extraction are not qualified.

`src/pe_arch.c` now owns Rufus's existing `MachineToArch` mapping and the bounded
`FindPeArch` byte parser. Windows `FindArch` retains native file mapping and passes
its checked file size into that parser. DOS/PE offsets and COFF headers must fit
the supplied span; unknown/truncated input returns `ARCH_UNKNOWN`. The original
machine list is unchanged, including unsupported ARMNT remaining unknown. This
does not validate the optional header, EFI subsystem, signature or bootability.
Windows source/build manifests are updated; native Windows validation is deferred.

The native component tests extract exact generated bytes from ordinary files
and RR/Joliet/UDF containers, including compressed/solid/pipable WIMs, fragmented
UDF and distinct filesystem views. They exercise the exact size limit, damaged
resource hashes, missing paths, cleanup, failures and cancellation, including
retrying the same WIM and using the borrowed reader after freeing it. The shared
parser has a standalone portable C test. See the
[checkpoint record](testing-and-checkpoints.md#cp3g4a-single-file-wim-extraction-and-pe-architecture-2026-09-14).

**Next integration boundary:** Rufus's `ImageScanThread` applies
`Windows/Boot/EFI/bootmgr.efi`, image 1, to **standalone Windows WIM/ESD inputs**
(`is_windows_img`). It does not infer an ISO's firmware bootability from that
file inside an installation WIM. Qualify standalone-WIM classification and
selected ordinary-file ownership before adding this result to SELECT. Explicitly
select/test Windows-compatible WIM path matching at host initialization, preserve
unknown/missing/unreadable distinctions, and keep Secure Boot pending. The Qt
form and existing SELECT metadata behavior are unchanged in CP3g4a.

## CP3g4b: standalone classification and native source ownership

`IsWimImageSignature` in `src/image_file.c` now shares the eight-byte `MSWIM`
signature check from Windows `IsBootableImage` in `src/vhd.c`. It requires eight
actual bytes, including all three trailing zero bytes. The Windows caller now
passes `ReadFile`'s returned count, avoiding classification from a partial prefix.
Its surrounding raw/compressed-image policy is unchanged. Windows source
manifests already include this shared module; native Windows validation remains
deferred and the Windows application's existing build systems remain in use.

This check identifies a candidate regardless of filename, including `.wim`,
`.esd` or a different suffix. It does not decode the archive, identify Windows
or prove bootability. An invalid archive with that signature remains a failed
WIM candidate. Conversely, wimlib's distinct pipable signature and an ISO/UDF
containing an installation WIM do not become standalone Windows candidates.

`linux/platform/image_source.c` owns a concrete descriptor and initial `stat`
result. It opens an ordinary file read-only/nonblocking/CLOEXEC, rejects special
files, and uses `pread` to obtain the signature without changing the offset.
Short reads and EINTR are handled, premature EOF is rejected, and cancellation
or changes during the read clear and close the output. A caller initializes
`fd = -1`, closes before reuse, and closes after all borrowers finish.

`OpenWimFileDescriptor` reuses the existing native `/proc/self/fd` opener. wimlib
obtains its own descriptor for the retained inode without moving the caller's
offset or taking ownership. The host initializes the library, frees the WIM,
checks the retained source and pathname, then closes the source before publishing.
`CheckImageFileSource` compares device, inode, size, mtime and ctime with nanosecond
precision; symlink retargeting and equal-size replacements are detected, as are
in-place writes with restored mtime. Atime is deliberately ignored. This detects
changes at the check, not an immutable snapshot or changes after that check.
No original-path reopening is allowed as a fallback.

One upstream limitation is retained: `wimlib_open_wim` obtains a filename with
`realpath`, even for read-only access. If the original inode was already unlinked,
opening through its held descriptor returns `WIMLIB_ERR_NO_FILENAME`. The caller
must reject that analysis; preserving a stale result has no benefit. Replacement
after wimlib opens still reads the original content and fails the final source
check. The existing five-file reader patch remains unchanged.

The native integration probe initializes wimlib with
`WIMLIB_INIT_FLAG_DEFAULT_CASE_INSENSITIVE` and qualifies mixed-case EFI lookup,
image-1 selection with boot index 2, compressed/solid archives and source changes.
Known EFI architecture does not invent missing Windows version fields. The
[checkpoint record](testing-and-checkpoints.md#cp3g4b-standalone-wim-classification-and-source-ownership-2026-09-14)
lists functional and ownership coverage.

**Next checkpoint:** wire this source into the existing Qt analysis job and
controller. Select Windows-compatible lookup once at host initialization before
workers run. Preserve separate candidate/decode/version/EFI states, final source
validation, cancellation and request-number checks. Test SELECT with generated
standalone WIM/ESD, then switching back to ISO and unknown inputs. Retain the
Win32 form, pending Secure Boot and the prohibition on deriving ISO firmware
bootability from an installation WIM's boot manager. CP3g4b changes no Qt behavior.
