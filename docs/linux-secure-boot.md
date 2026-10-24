# Secure Boot issuer and revocation checks

CP74 adds embedded FAT EFI enumeration and bounded local/system cached issuer
lookup with embedded precedence and deterministic signer fallback. See the
[ISO/EFI resource guide](linux-iso-efi-resources.md) for precedence, limits and
grouped evidence. This remains issuer metadata classification.

CP62 / Preview 31 delivers bundled-policy analysis and the START warning.
CP63 / Preview 32 adds [remote policy refresh and DBX caching](linux-policy-updates.md).
Complete Windows feature parity stays active.

## Windows reference

`src/rufus.c:GetBootladerInfo` visits the recorded EFI boot entries after image
analysis. `src/hash.c:IsSignedBySecureBootAuthority` classifies the immediate
issuer's SHA-1 certificate thumbprint against the active list. This is certificate
metadata classification; the Windows path does not authenticate the PE publisher
or verify the executable signature with `WinVerifyTrust` here.

`IsBootloaderRevoked` applies checks only when signer/issuer information exists,
in this order: PE256 hash in architecture DBX, Windows SSP hash list, Linux SBAT
generation, Windows SVN resource and expert-only certificate revocation. The
checked Windows source declares but never populates `pe256ssp`; the native policy
can consume an explicit SSP hash list and also defaults to empty. The fixed
minimum versions and active/revoked thumbprints were moved unchanged from
`src/db.h` to shared `src/secure_boot_db.h`; the Windows include and source/project
lists retain them. Original DBX resource bytes are reused for all seven
architectures, including the original empty IA64/RISC-V/LoongArch resources.

At START, any revocation bit causes an OK/Cancel warning before choosing a write
mode or authorizing a helper. SSP alone uses Windows recovery code `0xc0000428`;
other or mixed reasons use “Security Violation.” Cancel stops preparation. OK
continues to ordinary settings/device confirmation; it does not authorize writes.
Expert mode is sampled when loading the image, as on Windows, rather than
retroactively changing an already inspected selection.

## Native implementation

`linux/platform/secure_boot.*` uses the existing digest core and OpenSSL dependency.
It performs bounded PE32/PE32+ header, section, certificate, DBX, SBAT and resource
reads. PE256 follows the original ordered header/section/extra regions, skipping
CheckSum, the security directory and certificate tail. Bad offsets, truncated
records, overlapping hash ranges, zero divisors, excessive resource recursion
and malformed certificate/policy data return errors without publishing a result.
These checks replace the Windows parser's unchecked pointer accesses; accepting
malformed pointers is not behavior to preserve in the native reader.

Certificate extraction follows the first signer and immediate chain issuer,
falling back to the signer when no issuer is available. The chain comes from the
message's certificates, with no network or host trust-store lookup. Chain trust,
certificate dates and executable signature validity do not decide this metadata
classification, matching the role of the Windows function. This does not prove
that firmware will accept an image or that a publisher is authentic.

The ordinary analysis worker retains its ISO/UDF view and checks each recorded
EFI entry. It clears `ISO_PENDING_SECURE_BOOT` only when all reported entries
could be inspected. Missing/malformed files or embedded-only entries without
reported paths retain the pending indication. ISO-to-ESP/image eligibility,
source identity and write preflight remain separate checks.

`SelectionController` gates START before ISO or hybrid DD preparation; the
window displays the warning with Cancel as default. Acknowledgment is tied to
the image selection generation. Existing source and target checks still run
before the ordinary destructive confirmation and authorization. No helper
protocol or write semantics are changed.

## Evidence and remaining work

The grouped normal/sanitizer evidence covers PE digest exclusions, all five
revocation paths and precedence, issuer thumbprint classification, unchanged
bundled resources, parser bounds/cycles/cancellation, and the full image-analysis
suite. The warning UI case builds an ISO with a generated certificate-bearing SBAT-revoked
EFI fixture and checks actual worker reporting, Cancel, OK-to-confirmation,
zero authorizations and unchanged target bytes. It executes no EFI/Windows code.

The combined normal/sanitizer device routes pass, along with 2,196 real cases
in the one full Qt gate (five existing optional skips). Those include the
two older legacy-GRUB rows that failed in CP61; their previous failures remain
recorded, with no recurrence in this gate. Native/TGZ package inspection
passes; existing helper fault/polkit/claim evidence is reused. Command logs and current
source hashes live under `../.rufus-test-artifacts/cp62/`.

CP63 supplies remote SBAT/active/revoked lists and timestamp-selected DBX cache
updates. Broader certificate-chain availability, physical
firmware behavior, embedded EFI entry enumeration, other host/firmware
architectures and remaining legacy workflows stay in the full parity backlog.
