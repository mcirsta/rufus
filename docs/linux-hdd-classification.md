# USB HDD classification: reusable score and native facts

CP17d4 audit, 2026-09-26. References are the checked-out Rufus sources, not a
claim about every upstream release. No production eligibility changes here.

## What Windows actually does

`GetDevices` in `src/dev.c` calls `IsHDD` when List USB Hard Drives is off, except
for VHDs and detected cards. A score greater than zero hides the device. Zero
and negative scores pass this filter; other enumeration/protection checks still
apply. Cards use a separate limit: above **500 GiB** requires the option. VHDs
have their own exclusions. These are not all USB-HDD scoring cases.

`IsHDD` in `src/smart.c` uses the existing `src/hdd_vs_ufd.h` tables:

| Input | Current score rule |
| --- | --- |
| Drive type | Add 3 only for DRIVE_FIXED; removable and unknown add nothing. |
| Capacity | Below 128 GiB: subtract 15. Above 800 GiB: add 15; above 1800 GiB: add another 15. Exact boundaries matter. `GB` here means 2^30 bytes. |
| Friendly name prefix | First matching entry in `str_score`, case insensitive; preserve its current scan and wildcard behavior. |
| Name substrings | Add every matching `str_adjust` entry, case insensitive. Terms such as Flash and SSD can both contribute. |
| USB vendor ID | First matching `vid_score` entry. |
| USB vendor/product pair | First matching `vidpid_score` entry, in addition to the vendor contribution. |
| Diagnostics | When `usb_debug` is enabled, print the signed contributions, total and HDD/UFD interpretation. Logging does not change the score. |

The production scorer does **not** issue SMART or ATA passthrough commands.
Those helpers in smart.c are under RUFUS_TEST or disabled compilation blocks.
The two actual platform queries are `GetDriveTypeFromIndex` and `GetDriveSize`.
The former examines Windows drive types, falling back to disk geometry for
unmounted devices. It is not the earlier PnP removal-policy property.

The name comes from SPDRP_FRIENDLYNAME, with a generic fallback. Before scoring,
Windows has a UASP-specific replacement of its appended `SCSI Disk Device`
wording with `UAS Device`; the word SCSI itself contributes +20. Therefore a
Linux UI display name cannot automatically be treated as an identical input.
VID/PID come from the USB device ancestry; failed discovery starts at -1 and
reaches the scorer as uint16_t values. Do not manufacture a known USB ID from an
opaque UDisks object name or drive ID.

## Linux fact mapping

The installed UDisks interface contract was inspected at
`/usr/share/dbus-1/interfaces/org.freedesktop.UDisks2.xml`; libudev headers and its
installed API manual entries were also inspected. No host block device was
opened or interrogated.

| Scorer fact | Native source / unresolved distinction |
| --- | --- |
| Capacity | Validated whole-block UDisks `Block.Size`, revalidated by existing descriptor checks. Reject missing/zero size before scoring. The old Windows scorer alone treats a failed size query returning zero as a small device. |
| Fixed/removable media | Read the block device's native `removable` fact through its sysfs/libudev device. Keep unknown explicit. UDisks `Drive.Removable` describes user removability/hotplug; it is not Windows DRIVE_FIXED. `MediaRemovable` can be overridden for USB thumb drives, so it is not an exact substitute either. |
| Model/vendor | Native block/SCSI vendor/model plus UDisks vendor/model can supply a candidate description. Qualify ordering, padding and transport-generated suffixes against explicit fixtures before choosing the score input. Keep display-name construction separate. |
| USB VID/PID | libudev: resolve the block device by dev_t, then its nearest `usb`/`usb_device` parent and strictly parse `idVendor`/`idProduct`. The UDisks Drive interface has no dedicated VID/PID properties. Verify device identity and disk sequence around acquisition; do not add privileged enumeration. |
| Card status | Inspect native card facts/UDisks media compatibility separately. Do not equate MediaRemovable with a card, nor apply HDD scoring to all devices that Windows exempts as cards. Non-USB card readers remain another device-class task. |

libudev is already installed on this development host; it is a candidate native
adapter dependency, not yet a new build/runtime requirement. A small adapter
returning facts is sufficient. No Wine, SMART library, Windows enumeration
emulation, user password or disk write is needed to collect these facts.

## Extraction boundaries and protections

1. Move the score arithmetic/table matching to one portable C implementation,
   keeping `hdd_vs_ufd.h` as the single table source. Retain Windows fact gathering
   and log presentation in its thin `IsHDD` wrapper. Avoid platform conditionals
   throughout the scoring loop or a second Linux scoring algorithm.
2. Keep current arithmetic, first-match order, additive adjustments and quirks
   during extraction. ASCII case behavior is covered by the reference harness;
   do not claim that its POSIX shims validate Windows locale-sensitive APIs for
   arbitrary non-ASCII names. Resolve string-helper semantics explicitly.
3. Qualify a small Linux fact adapter read-only before replacing default-list
   eligibility. Missing native facts must not silently become known removable
   media or successful identity checks. Keep existing USB/system/source/whole-
   device/size/read-only gates independent of the score.
4. Keep the checkbox as an explicit user override of HDD filtering. A negative
   score must not be encoded as user opt-in or turn physical fixed flags into
   removable flags. Separate display eligibility from acquisition requirements.
5. Default-visible fixed-media flash devices must still use the protected helper,
   even when the checkbox is off. Native system/source/home/holder checks must
   run on that route with empty UDisks mount metadata. Requalify both helper
   acquisition and GUI routing before enabling the expanded default list.

Examples proven from the original function: an unknown-model 32 GiB fixed device
scores -12 and passes the Windows HDD filter; an unknown-model 2000 GiB removable
device scores +30 and is hidden. Current Linux's two-removable-flags filter can
therefore differ in **both** directions. Simply permitting all fixed disks or
adding the score on top of that strict flag filter would not establish parity.

## Reference behavior captured before extraction

The CMake target `test_hdd_score_reference` initially compiled the original
`IsHDD` function verbatim from the working tree. CP17d5a freezes that function in
`linux/tests/hdd_score_reference/original_hdd_score.inc` and compares it with the
current Windows wrapper and shared scorer, using the original tables, test-only
fact providers and ASCII string shims. It has no Windows headers, real
enumeration, Qt GUI or device I/O. The current wrapper is copied to the build
tree; changes to its signature or extraction boundary fail the build for review.

Thirty-two cases each run with diagnostics off and on: strict capacity boundaries,
unknown/fixed/removable types, prefix/substring/case behavior, additive USB ID
exceptions, all adjustment terms and debug-buffer capacity. Two legacy details
are deliberately recorded rather than silently corrected:

- A table entry longer than the model string ends the prefix scan, even though
  later entries can be shorter. `PNY USB` currently scores zero from names,
  while `PNY USB Device` receives -15.
- The `#` wildcard tests `strid[mlen]`, after the wildcard position. `ST1` and
  `ST1A` receive no model boost; `STX1` and `ST1000` receive +10.

These tests establish an extraction reference, not real Windows validation, new
production device eligibility or a hardware classification accuracy claim.

## CP17d5a — shared scorer extracted (2026-09-27)

`src/hdd_score.c` now owns the unchanged scoring rules and reads the existing
`hdd_vs_ufd.h` tables. It returns the total and ordered contributions. Windows's
thin `IsHDD` wrapper still queries drive type then capacity, once each, and emits
the original diagnostic text. The existing Windows build files include it.

Two small string headers selected by build include paths preserve Windows's
`_strnicmp` and `StrStrIA` calls and use Linux's `strncasecmp` and `strcasestr`.
The scoring loop has no platform branches. Linux CMake builds the shared library;
discovery does not use it yet. Non-ASCII locale equivalence remains unverified.

All 32 expected cases and 5,496 table combinations match the frozen original,
including full diagnostic text with debug off/on, normally and under sanitizers.
The frozen function matches the pre-extraction `smart.c` baseline verbatim
(baseline file SHA-256:
`bd7b96ac3c9e308c56f9d63726a74670fc7886b8bcc5c060ceb76a568ee772c7`).
See the testing record for the validation limits.

Next is CP17d5b: qualify the read-only native fact adapter before enabling any
expanded default device eligibility. The protection requirements above remain.

## CP17d5b — native disk snapshots (2026-09-27)

`linux/platform/native_disk_facts.cpp` supplies read-only facts from sysfs,
following the backend's existing direct sysfs approach. This implements the
native snapshot portion of CP17d5b; friendly-name mapping and card exceptions
remain for CP17d5c. No libudev dependency is needed for these attribute reads.

The adapter resolves a supplied device number beneath `/sys/dev/block`, verifies
its block subsystem and whole-device status, checks capacity against the caller's
expected size, and requires a nonzero disk sequence. It rechecks device number,
capacity, sequence and lookup path after reading optional facts. Invalid identity
returns an error and clears all output. Sysfs capacity uses 512-byte sectors,
independently of the device's logical sector size. This snapshot is not permission
to write or a replacement for descriptor-based acquisition checks.

It reads native fixed/removable status with an explicit Unknown state. USB IDs
come from the nearest USB device ancestor, skipping USB interfaces; each must be
exactly four hexadecimal digits. Missing or malformed IDs stay unknown and never
fall back to the enclosing hub's IDs. Raw vendor/model byte strings remain
separate and have no manufactured Windows transport suffix. Missing, oversized,
NUL-containing or unreadable attributes yield unknown optional facts. Reads are
bounded; no device node, SMART command, password or disk write is involved.

Thirty Qt test checks pass normally and with ASan/UBSan/leak detection. Fixtures
cover nested USB topology, IDs, optional unknowns, partitions, identity/size and
sequence rejection. A FIFO coordinates a device change during fact collection;
sequence, capacity, dev_t and lookup-target changes each discard the snapshot.
Fixtures live in automatically removed temporary directories. These are simulated
sysfs checks, not physical USB or cross-platform friendly-name qualification.

The GUI does not consume this adapter yet. Missing disk sequence is a failed
snapshot, not a guessed identity; older-kernel fallback behavior remains to be
decided during integration. Native card-reader classification, the Windows
friendly-name equivalent (including USB/UAS naming), and reuse of UDisks card
facts are next. Only after those rules and protected acquisition are qualified
may shared scoring replace the existing default filter.

## CP17d5c — native score inputs and card policy (2026-09-27)

`EvaluateNativeDiskFilter` extends the existing native-facts adapter and calls
`GetHddScore`; it introduces no second scoring algorithm or new adapter library.
It evaluates only the default HDD/card filter. Include is not proof of device
eligibility or permission to write. Native removal facts, user checkbox opt-in
and the filter result remain separate.

The score input is the observed, trimmed native vendor followed by the model,
with one separator when both exist. Missing components remain missing; an empty
name supplies no name evidence. There is no display-name fallback, manufactured
Windows transport suffix or removal of a genuine SCSI term. Known USB pairs pass
through unchanged; unknown pairs use Windows's failed-lookup sentinel (0xffff).
Unknown native fixed/removable status returns Unknown for HDD scoring rather
than pretending the device is removable. A zero size also stays Unknown.

This is a deliberate native mapping, not proven Windows friendly-name equality.
For example, `PNY` + `USB` retains the original scorer's short-name result; the
adapter does not append `Device` to force another result. A missing Windows-added
suffix can therefore change the result. Using observed facts avoids inventing a
transport/name that Linux did not report. Non-ASCII locale equivalence and actual
cross-platform hardware-name comparisons remain unqualified.

Card policy uses the installed UDisks Drive XML contract's explicit identifiers:
`flash`, `flash_cf`, `flash_ms`, `flash_sm`, `flash_sd`, `flash_sdhc`, `flash_sdxc`,
`flash_sdio`, `flash_sd_combo` and `flash_mmc`. `thumb` denotes a USB stick and
does not take the card exception. Current nonblank Media takes precedence;
MediaCompatibility is used when current media is unknown/blank. No arbitrary
`flash_*` prefix, display-name substring, Removable or MediaRemovable flag
establishes card status. Unknown identifiers do not gain the exception.

Positive card metadata bypasses HDD scoring, as Windows's is_CARD branch does.
The existing `MAX_DEFAULT_LIST_CARD_SIZE` definition moved to `src/hdd_score.h`;
Windows still uses that same macro through `rufus.h`, and Linux uses it directly.
The value is unchanged: 500 GiB is included, larger cards require the explicit
hard-drive listing option. Native metadata differs from Windows PnP enumerator
and hardware-ID detection; neither enumeration nor safety gates are emulated.

All 66 native-facts/policy Qt checks pass normally and under ASan/UBSan/leak
detection. Seventeen cases read fake sysfs files through the actual snapshot
reader and feed their results into the original scorer. They cover ordering,
padding, missing names, fixed/removable/unknown, genuine SCSI text, legacy short
names and known/unknown USB pairs. Card cases cover every listed identifier,
non-card/future/malformed tokens, current-media priority, unknown removal and
independent 500-GiB boundaries. The original-function differential target still
passes 32 expected cases and 5,496 table cases with diagnostics off/on.

This adapter remains outside the GUI/helper production dependency graphs until
integration is qualified. Current device-list eligibility is unchanged. No host
device access, Windows runtime test, VM or customization qualification was needed.

**Next: CP17d5d, integrate fresh classification and protected acquisition.**
Apply the policy consistently to discovery and fresh preflight/helper snapshots.
Keep whole-device/USB/system/source/read-only gates independent. Derive native
protection requirements from verified physical facts, not just checkbox opt-in:
a fixed-media flash drive admitted by a negative score still needs the existing
source/home/system/holder checks when UDisks reports no mounts. Qualify these
paths before exposing the expanded default list; use the same authorized helper.


## CP17d5d — discovery and protected acquisition integration (2026-09-27)

`DescribeUDisksDevice` now reads the verified native USB snapshot and calls the
existing shared filter. Discovery, raw preflight and the authorized helper use
that same function. Whole-device, USB, writable, media-available and protected
UDisks gates remain independent; malformed removal metadata still fails. The
hard-drive listing option bypasses only the HDD/card filter, including Unknown,
not failed native identity/size/sequence reads. Missing disk sequence now also
excludes discovery; these devices could not meet the existing helper write
contract. A secure older-kernel fallback remains unimplemented.

`Device.defaultVisible` carries the default-filter result through Qt selection
and raw-plan preparation. Physical removal flags and `allowFixedUsb` retain their
original meanings. `requiresNativeProtection` comes from native and UDisks facts:
only agreement that both drive and media are removable can avoid the fixed-media
checks on an unmounted target. A fixed or unknown native fact requires them even
if the scorer includes the disk by default. The helper recomputes these facts;
it does not accept a GUI-provided protection flag.

Selection identity now includes canonical native path, disk sequence, removal,
USB pair, lossless vendor/model bytes and UDisks removal/card properties. Native
changes invalidate a selection even if a refreshed snapshot still passes the
filter. Filesystem labels/content remain outside identity. Existing source and
service-owner checks remain in place. No scoring algorithm, authorization route,
write engine or Windows source was added or changed in this checkpoint.

The helper invokes its existing source/home/system/holder/mount protections
before acquisition and repeats them after claiming the disk, both for opt-in
and default-visible fixed media. Empty UDisks mounts cannot bypass this path.
The same single helper owns unmounting, writes, sync, cancellation and exit;
READY still waits for completion. Existing FUSE/layered backing limitations
remain; accepting a score does not imply those layouts can be safely resolved.

Tests with invented UDisks device numbers link a test-only native-reader wrapper.
Production and guest binaries always use real sysfs; the adapter's separate
filesystem fixtures still test actual native reads. Integration tests cover
fixed flash, disagreements in removal facts, large removable disks, Unknown,
card limit/current-media precedence, snapshot failure with opt-in, native identity
changes after selection and before START, and old D-Bus replies. Large SFD GUI
fixtures now explicitly select the hard-drive option, as their size score requires.
The generated fixed-USB VM uses real sysfs and the production helper; its initial
seed/source setup now supplies a guest caller and independent tmpfs backing.
See the checkpoint testing record for results and limits.
