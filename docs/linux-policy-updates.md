# Secure Boot policy updates

CP63 / Preview 32 adds remote policy refresh to the image checks introduced in
[CP62](linux-secure-boot.md). Open **Settings** to choose Disabled, Daily,
Weekly or Monthly, run **Check now**, or cancel an active check. The first
application launch asks before enabling automatic network checks. The default
answer is No; accepting uses the original daily interval.

When enabled, the three small Rufus policy lists refresh once per application
session. DBX checks follow the selected interval. The lists cover Linux SBAT,
active Secure Boot issuer certificates and revoked certificates. A newer DBX
prompts once per check before downloading any database bodies. Declining stops
the DBX portion; valid list updates already received remain available.

Updated policy applies to the next image selection. Select an existing image
again to recheck its EFI loaders. An analysis already running retains its own
policy snapshot. These downloads update Rufus's image checks. They do not write
firmware variables, authorize device creation or replace target firmware trust.

## Windows reference and native behavior

`src/stdlg.c:CheckForFidoThread` fetches `sbat_level.txt`, `sb_active.txt` and
`sb_revoked.txt` from `https://rufus.ie/` during enabled sessions. Native lists
also remain in memory, with the shared bundled text available at startup.
The original `UpdateCheckInterval` preference supplies disabled/daily/weekly/
monthly/custom values. Linux records its DBX schedule separately as
`LastPolicyUpdateCheck`, so a session-only list refresh cannot postpone DBX.

`src/net.c:CheckForDBXUpdates` uses the seven entries in
`res/dbx/dbx_info.h`, each GitHub commit's UTC author timestamp and a single
Yes/No decision before downloading newer files. Native code shares those
unchanged URLs/timestamps. Original empty architectures with no remote file
remain unavailable without making an otherwise successful check fail.
The original SSP list has no population path in this source tree; no host SSP
inference is added.

Windows stores raw `dbx_<arch>.bin` files with separate registry timestamps.
Linux instead stores `dbx_<arch>.json` in the selected application files folder,
using one atomic envelope containing architecture, timestamp, original bytes
and SHA-256. This explicit difference prevents an interrupted replacement from
pairing a new database with an old timestamp. A failed replacement preserves
the previous valid cache. The existing normal/portable storage choice applies.
The bundled database remains the fallback; only a validated cache newer than
the bundled timestamp replaces it. Cache hashes detect storage corruption;
they do not establish publisher authenticity.

The cache accepts owned regular single-link files, refuses symlink destinations,
checks retained-file identity, bounds encoded/decoded sizes, and validates the
DBX signature-list structure before use. Lists require nonempty parsed records
under the original 1,024-byte bound. Malformed, unavailable or oversized
responses retain the current valid policy. Cancellation preserves completed
validated updates and aborts the active response.

The shared Qt HTTP transport also serves existing signed payload downloads.
It bounds responses, handles redirects/TLS/network errors and inactivity,
and publishes one final result. Signed downloads still require exact lengths
and verify the original detached signature before accepting payloads. Policy
feeds use HTTPS and structural validation as in Windows; a cache checksum is
not a detached signature. Production endpoints do not accept arbitrary HTTP;
loopback HTTP is available for local test fixtures.

## Qualification and remaining scope

The focused normal and sanitizer group covers the retained signed-download
suite, policy schema/cache/UTC timestamps, offline and malformed responses,
chunked and oversized bodies, one DBX decision, cancellation including a
reentrant progress callback, GUI settings/consent, and frozen analysis policy.
Public-feed samples record endpoint compatibility without executing downloads.
Both combined native guest routes pass. The one full Qt gate passes 2,198 real
cases with five existing optional skips and one obsolete tab-order assertion.
The test now includes enabled Settings; all 87 window cases pass normal and
sanitizer reruns. Across the gate and repair runs 2,199 distinct cases pass;
the initial failure is retained. The focused group passes 118 cases per build.
Exact commands, outcomes, package hashes and cleanup are recorded under
`../.rufus-test-artifacts/cp63/`.

Application binary update discovery and installation, image download selection,
remaining payload providers, localization and all other unresolved Windows
features remain in the full parity goal. Broader certificate-chain availability,
embedded EFI enumeration and physical firmware qualification retain the limits
described in the Secure Boot guide. This batch changes no helper protocol or
device-write semantics; existing fault/polkit/customization evidence is reused.
