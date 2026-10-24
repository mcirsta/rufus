# Native application update checks — CP68 / Preview 37

Settings now includes native application release checks alongside the existing
Secure Boot policy controls. **Check now** checks both services; **Cancel check**
cancels both transfers. Application checks need no device/helper authorization.
They report a newer release in Settings, with an **Open release page** action.
Automatic discovery waits until media work and modal dialogs finish before
showing a default-Close notification. Release notes are plain text.

Windows obtains a signed `.ver` file from Rufus, compares its three unsigned
version words and platform minimum, then offers the signed Windows executable
for replacement. Linux uses the port publisher's native releases and a release
page/package-manager handoff. Installed native packages own multiple executable,
helper, policy and documentation files plus package database entries. Updating
those consistently requires the distribution's package mechanism. A Windows
`.exe` cannot update those native files. The native application consequently
opens the publisher's release page after an explicit action; it neither claims
package installation nor manages the distribution's trust keys.

## Source, versions and channels

The configured publisher is the port's existing origin,
[`mcirsta/rufus`](https://github.com/mcirsta/rufus/releases). Checks use its public
HTTPS GitHub release API and accept exact native CPack archive or Arch package
names produced by this project. Selection uses the compiled architecture; no
host settings are inferred. Windows binaries and other architectures are ignored.
Version triples use Windows's unsigned-word ordering. Native preview revisions
are compared numerically, and a stable release follows previews of its triple.
Conflicting native versions within a release are refused.

Drafts are ignored. The original **Include beta versions** Yes/No choice uses
`CheckForBetas`, defaults to No, and follows Windows's x64 eligibility. Disabled
intervals disable that preference control. **Alt+Y** toggles the original
session-only forced-update behavior: an eligible published release can be
offered even if its version is equal/older. It preserves architecture/channel
eligibility and performs no network request merely from toggling the shortcut.

Metadata is bounded to 100 releases/page, five pages, 128 assets/release, 4 MiB
per response and 32,768 release-note characters. A full scan that reaches the
page limit fails explicitly. Release pages must match the configured publisher
and tag on `https://github.com`, with no alternate origin, credentials, query or
fragment. Invalid native version/asset metadata and partial catalogs remain
errors. Existing bounded HTTP transport retains its TLS and redirect checks.
HTTP server dates are parsed in explicit English/UTC before timestamps are saved.

These checks discover release metadata and offer a web page. They do not validate
or download a native executable/package. Package verification and installation
belong to the selected distribution workflow. The original Windows download
signature key continues to protect existing Windows-owned resources; it is not
presented as a signature on this fork's native packages.

## Consent, scheduling and errors

The shared Disabled/Daily/Weekly/Monthly/custom interval remains
`UpdateCheckInterval`. New first-run consent explicitly names application and
Secure Boot updates. Existing profiles that consented only to Secure Boot retain
that narrower choice: `ApplicationUpdateConsent` defaults to false. The explicit
**Automatically check for application updates** checkbox enables scheduling.
Manual **Check now** is a one-time request and leaves that preference unchanged.

Successful checks save their server time as `LastApplicationUpdateCheck`, separate
from Secure Boot and Windows-channel timestamps. Missing/invalid dates, HTTP or
catalog errors preserve the previous successful timestamp and release nothing.
Automatic failures back off for the chosen interval; long timer ranges retain
the remaining wait. Clock corrections backwards can recover, and Windows's
clock-drift diagnostic is retained. No background service is installed.

The live API check on 2026-10-21 returned an empty release list. The real checker
successfully reports **No published Linux release matches this architecture and
update channel**. It does not call that outcome “up to date.” Publishing native
releases and qualifying distribution installation remain delivery work. Local
fixtures prove available/current/forced, beta, pagination, rejection, cancellation,
notification and release-page offer behavior without publishing anything.

## Related application fixes and qualification

The Windows stale-translation log notice is now emitted for older catalogs.
Repeated/initial Qt language events retain already translated dialogs when the
selected language has not changed. This fixes a newly opened settings dialog
being discarded by an initial queued language event. Actual language changes
retain the CP67 state/log-preserving relabeling behavior.

One grouped normal and ASan/UBSan/LSan run checks the release reader, real loopback
HTTP, scheduling/consent, settings and notification routes, plus the existing
localization and shared Secure Boot policy checks. Combined diskless,
networkless guests repeat the application group as UID1000 with loopback fixtures.
One full nine-suite Qt gate and inspected native/TGZ Preview 37 packages complete
the batch. Existing writer/helper/boot/fault/polkit and agreed customization
evidence is reused. No host media is written or mounted and no package is
installed. Logs, compact guest evidence, manifests and hashes are retained in
`../.rufus-test-artifacts/cp68`; owned guest/package stages are removed after export.

Full Windows parity remains active across native translation coverage,
distribution-backed update delivery, remaining settings/shortcuts and all other
unresolved feature/platform groups.

## CP78 — first public native preview

The owner requested GitHub tag/release `linux-preview.47` at `mcirsta/rufus`,
with native Arch/CachyOS, Debian13 and Fedora44 packages and a generic install
tree. Enable **Include beta versions** to discover this preview. The AUR source
recipe pins the downloaded GitHub tag archive; the owner submits to AUR manually.

The dated CP68 empty-catalog observation above is historical. The source candidate
contains an opt-in `publishedRelease` qualification hook: after publication,
load the actual saved public release-list response, select Preview47's supported
x86-64 asset/version/channel and verify the explicit release-page button handoff.
The post-tag result, source archive hash and package hashes belong to the final
attached CP78 delivery receipt, avoiding a self-referential source-tag checksum.
Package installation remains with the distribution package manager.

## CP72 — advanced controls

Ctrl+Alt+Y toggles the native forced-release behavior and logs the Windows-specific
Authenticode timestamp boundary when checking. Windows's override applies to the
downloaded executable signature, not the HTTP Date header. Native package
verification remains with the distribution workflow. Forced checks still refuse
invalid HTTP dates and TLS/origin errors. `VerboseUpdateCheck` levels 1 and 2 add
request/result and detailed parameter/response logs respectively. See
[desktop controls](linux-desktop-controls.md) for exact behavior and qualification.
