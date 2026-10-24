# Rufus download signatures — CP21o/CP21p/CP21q

Audited and qualified 2026-09-30 against this checkout's `src/pki.c` and
`src/net.c`. This checkpoint supplies a verifier, before network/prompt/cache
integration. It applies to Rufus's detached bootloader-download signatures;
Windows executable Authenticode and script validation remain separate paths.

## Original format and trust root

`DownloadSignedFile` downloads the payload and `<URL>.sig` into memory. It
requires a nonempty payload and exactly `RSA_SIGNATURE_SIZE` (256) signature
bytes. `ValidateOpensslSignature` hashes the exact payload with SHA-256 and
verifies RSA-2048 PKCS#1 v1.5 using the embedded modulus and exponent 65537.
Only validated data reaches the file-creation/write branch. Verification failure
sets the existing signature error; there is no unsigned installation branch.
The GRUB prefix search may try further candidate URLs, and ordinary exhaustion
retains the existing embedded-fallback contract.

The original 257-byte big-endian modulus includes an ASN.1 leading zero. Windows
reverses its last 256 bytes into a CryptoAPI `PUBLICKEYBLOB` and reverses the
raw signature in place. Those conversions serve CryptoAPI's representation;
the server signature is already in OpenSSL byte order. Microsoft documents
that [CryptoAPI and OpenSSL use opposite signature byte order](https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-cryptverifysignaturea).

The key now lives once in `src/download_key.h`, with its original bytes and
provenance comment retained. Windows includes that header, uses the shared
length/exponent and keeps its existing verifier/downloader. Its size constant
is checked at compile time against the shared one. Visual Studio and
Autotools distribution metadata include the header; no OpenSSL dependency or
Linux branch was added to Windows. The modulus matches the pre-change source
byte for byte; SHA-256 is
`de1a250288df6f007af1c82a2f10ffa6c23f853d595b6fc9235727bbf967df45`.

## Native adapter

`linux/platform/download_signature.c` is a 91-line C adapter using the system
OpenSSL 3.0+ Crypto library. It imports the shared integer/exponent through
[OpenSSL's provider key importer](https://docs.openssl.org/3.0/man3/EVP_PKEY_fromdata/)
and explicitly selects SHA-256 and PKCS#1 v1.5 in the
[EVP verification API](https://docs.openssl.org/3.0/man3/EVP_DigestVerifyInit/).
OpenSSL performs the cryptography; no custom RSA, hash, padding, parser or trust
store was introduced. Production never invokes the `openssl` executable.

The interface accepts accessible immutable byte spans and returns an errno
result: zero only for a valid signature, `EBADMSG` for an invalid signature or
signature length, `EINVAL` for invalid/empty payload arguments, `ENOMEM` for
explicit object/parameter allocation failure, and `EIO` for crypto provider or
setup/operation failure. It releases owned key/context/parameter objects on
every path and never retries using a weaker hash, padding or another key.
Buffers are neither reversed nor zeroed. Windows zeroes bad payloads and later
frees them; the native caller must discard rejected bytes before publishing or
caching them. Returning a rejection preserves that acceptance boundary without
modifying caller-owned data.

The C target is linked through the existing source library for future download
use. CMake locates `OpenSSL::Crypto`, and Linux build documentation/CI record the
development dependency. This does not wire cached cores into the helper or
change the original unsigned existing-cache contract from CP21m/n.

## Evidence and limits

One 35,918-byte GRUB 2.12 payload and its 256-byte signature were obtained from
the actual Rufus `FILES_URL` hierarchy and independently verified with the
OpenSSL command line. They are checked in under
`linux/tests/fixtures/download/grub-2.12`, with URLs, lengths and hashes. Tests
use these files offline. Their presence does not imply module compatibility
with any ISO; no downloaded boot code was executed in this checkpoint.

The 36-scenario functional suite passes normally and under ASan/UBSan/LSan.
It checks authentic data, mutations at multiple payload/signature positions,
truncated/extended/empty inputs, reversed signature order, invalid lengths,
null arguments and correctly formed signatures made with an unrelated key.
It also tests allocation, import, digest/padding setup and verifier failures,
an actually unavailable provider, buffer immutability and same-process recovery
after rejection/setup/provider failure. The independent CLI verifier is a
reference for authentic bytes, not the implementation used in production.

The 48-case cache reader and 144-case shared GRUB inspection regressions also
pass normally and under sanitizers. Linux GUI/helper builds pass; Windows XML,
line endings and distribution metadata were checked, but Windows compilation
and execution remain deferred. Small evidence is in parent
`.rufus-test-artifacts/cp21o`; disposable builds, temporary fixture copies and
test-only private keys were removed. The two small checked-in fixture files
remain intentionally for repeatable offline tests.

CP21p is a bounded unprivileged Qt Network fetcher for the payload/signature
pair, with verified owned bytes as its output. Controlled local HTTP tests
should cover exact bytes, status/read failures, bad signatures, size limits and
cancellation without depending on the public server. Prompt/cache installation,
full-name/prefix search integration, frozen helper transfer and a compatible
image/core/module boot follow separately. A valid signature establishes the
fixed key signed those bytes, not a module ABI, image version or bootable ISO.


## CP21p — bounded Qt Network fetcher

Completed 2026-09-30. `linux/qt/signed_download.cpp` adds one 230-line,
unprivileged, asynchronous adapter. Qt handles HTTP, TLS, redirects and socket
lifetime; OpenSSL handles signature verification using the shared Rufus key.
There is no authored HTTP parser, blocking worker, custom cryptography or
additional host detection in production. Named Qt slots keep cancellation and
completion in the object's event-loop thread.

### Contract retained from Win32

Each candidate requires HTTP 200, a positive declared Content-Length and an
exact completed body. Payload size is limited by the caller's embedding gap
and a 1-MiB ceiling; the detached signature must be exactly 256 bytes. Invalid
lengths are rejected before reserving the body buffer. Requests ask for identity
encoding and reject transformed content. Missing-length/chunked responses are
not silently accepted. The signature URL is the original encoded payload URL
with `.sig` appended, even when Qt redirects the payload to another origin.
The API accepts plain HTTP(S) file URLs without credentials, query or fragment,
as used by the bootloader server's candidate paths.

The idle timeout defaults to Win32's 3,500 ms. Headers/data restart it; it is an
inactivity limit rather than an absolute transfer deadline. Qt limits redirect
chains and throttles reply buffering with a 64-KiB read buffer; our accumulated
body stays within the validated length. This does not assert an exact ceiling
on all Qt transport allocations: Qt documents that the reply buffer limit is
not a precise bytesAvailable guarantee in all protocols.
[Qt reply buffering](https://doc.qt.io/qt-6/qnetworkreply.html#setReadBufferSize).

Only verified completion returns an owned payload and SHA-256 fingerprint.
Errors and cancellation return empty bytes/hash, an errno reason, the actual
last HTTP status (when present), and the payload/signature stage. That stage
allows later UI code to retain Rufus's missing-signature warning behavior
without losing the original transport failure. This result is not the Windows
global DownloadStatus. Transfer progress covers payload bytes only: 100% is
not verified completion. Each accepted start emits one terminal completion;
invalid/busy starts emit none. Completion makes the object idle before emitting,
so a receiver may restart or delete it. Destruction silently aborts pending I/O.

### Deliberate transport difference

Windows `DownloadToFileOrBuffer` permits HTTPS-to-HTTP redirects through its
WinINet flags. Linux uses Qt's NoLessSafeRedirectPolicy and refuses that
downgrade, preserving HTTPS transport protection. This is an explicit security
difference, not an implementation shortcut. Ordinary trusted HTTPS and safe
cross-origin redirects remain usable. Certificate/handshake failures stop;
there is no ignore-errors path, authentication UI or caller-selectable trust
root. Cookies and Qt disk-cache reads/writes are disabled for these downloads.
[Qt redirect and request policies](https://doc.qt.io/qt-6/qnetworkrequest.html).

### Qualification and remaining integration

All 63 Qt cases pass with real loopback TCP/TLS servers, normally and under
ASan/UBSan/LSan without suppressions. The servers replay CP21o's small authentic
payload/signature; no external download or USB is used. Tests exercise fragmented
transfer, exact gap limits, huge declared lengths, HTTP errors, malformed or
missing lengths, short reads, encoding, bad/missing signatures, native verifier
failures, timeout at either stage and cancellation during body/progress/signature
retrieval. They check exactly one completion, empty failed results, reply
cleanup, reentrant restart, receiver deletion and destruction during a request.

Cross-origin redirects use the original signature URL, do not carry cookies,
and preserve encoded paths. Redirect loops are bounded. Real TLS tests reject
untrusted certificates and a trusted HTTPS-to-HTTP redirect; trusted HTTPS
succeeds. Only tests install their temporary certificate as a test trust root;
production trust is unchanged. Temporary private keys/certificates are removed
by QTemporaryDir.

The 36 signature, 48 cache and 144 inspection cases also pass normally and under
sanitizers; Linux GUI/helper targets build. No Windows source/build behavior
was changed in this checkpoint; actual Windows validation remains deferred.
Compact evidence is in parent `.rufus-test-artifacts/cp21p`. Disposable builds
were removed after export.

Cache installation, full-name/prefix selection and Yes/No/Cancel integration
remain separate steps. Verified bytes must then be frozen for the existing
single authorized helper and qualified with a compatible image/core/modules/
configuration boot. Neither a valid signature nor an existing unsigned cache
entry establishes image/module compatibility. Image-backed GRUB creation remains
disabled pending that complete path.


## CP21q — atomic full-version cache installation

Completed 2026-09-30. `StoreGrub2CoreCache` is added to the existing cache
component, sharing its version/path/gap validation with the reader. It is an
unprivileged storage operation: its caller must supply only bytes accepted by
`SignedDownload` with `error == 0`. It does not authenticate arbitrary inputs,
re-verify signatures or introduce a new trust/token abstraction. The API leaves
the input report and owned bytes unchanged.

Windows creates `grub-<full detected version>` before searching shortened server
candidates, and writes every successful candidate as `core.img` in that full
name's directory. Linux retains that naming rule. Cache storage never changes
the detected version or asserts that a shorter signed candidate matches the
ISO's module ABI. Existing unsigned caches retain their original reader contract.

The writer validates the shared BIOS/MBR lookup gate, absolute directory, safe
full version and embedding gap before creating directories or opening a file.
It accepts 512 bytes through the available gap (at most 1 MiB minus the MBR
sector), writes in 64-KiB chunks and checks the existing cancellation query
before/between writes and before commit. `QSaveFile` handles temporary-file
cleanup and atomic replacement; direct-write fallback is explicitly disabled.
[Qt's atomic file replacement](https://doc.qt.io/qt-6/qsavefile.html).

This deliberately improves Win32's direct cache-file write: a failed save cannot
leave a truncated bootloader for the next run to read. A previous cache remains
intact on write/open/commit failure or cancellation observed before replacement;
without a previous cache the final file remains absent. Empty newly created
directories may remain. Successful commit is the operation's completion point;
later cancellation does not roll it back. No absolute power-loss/directory-sync
durability guarantee is claimed for this disposable cache, and this operation
is separate from the target-device sync required before READY.

The 45-case storage suite passes normally and under ASan/UBSan/LSan. It stores
the authentic signed fixture under a suffixed full name and independently
reads it with the existing cache reader. It checks exact minimum/maximum gaps,
report/buffer immutability, owned readback, invalid inputs, real directory/file/
permission failures and no partial final file or leftover temporary file.
Test-only link wrappers force open, short/failed first/later writes, commit
rejection and cancellation; real Qt temporary-file cleanup/commit paths still
run. Old contents are read while replacement is pending, and canceled saves
can be followed by successful saves. Synthetic storage bytes test geometry and
failure handling, not authenticity or bootability.

All five focused CTests pass normally and under sanitizers, including the
63 download, 36 signature, 48 cache-reader and 144 inspection cases. GUI/helper
builds pass. Small evidence is in parent `.rufus-test-artifacts/cp21q`; temporary
builds were removed after export. No USB, privilege helper job, additional
customization test or Windows execution was used.

CP21r connects cache-first lookup, the Win32 Yes/No/Cancel decision and shared
full-name/dot/dash download order in one unprivileged selection flow. A save
error must remain a failed candidate, preserving the original fallback/error
policy rather than silently selecting a download whose installation failed.
Cancellation and stale image selection stop the flow. Frozen helper transfer
and a compatible image/core/module/configuration boot still follow; image-backed
GRUB creation remains disabled until those are qualified.
