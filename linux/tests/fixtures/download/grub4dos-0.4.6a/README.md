# Signed standalone GRUB4DOS fixture

Fetched 2026-10-04 from Windows Rufus's original FILES_URL hierarchy:
[grldr](https://rufus.ie/files/grub4dos-0.4.6a/grldr) and its
[signature](https://rufus.ie/files/grub4dos-0.4.6a/grldr.sig).
`provenance.json` records exact lengths and SHA256 values. The loader is 322,965
bytes and contains the 0.4.6a 2024-02-26 banner matching the bundled resource's
release. GRUB4DOS is GPL software; upstream source and license are available at
[the official release](https://github.com/chenall/grub4dos/releases/tag/0.4.6a).

Tests verify the untouched binary/signature with the original Rufus public key,
then serve it on private loopback sockets. There is no fixture signing key or
trust override. Cache/I/O/storage fixtures also use synthetic unsigned bytes to
exercise bounds and failures; these do not claim authenticity or bootability.
This checkpoint does not execute or install grldr, or establish an ISO's version.
