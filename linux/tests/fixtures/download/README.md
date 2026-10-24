# Signed Rufus download fixture

Fetched 2026-09-30 from the same `FILES_URL` hierarchy used by Windows Rufus:

- [GRUB 2.12 core](https://rufus.ie/files/grub-2.12/core.img): 35,918 bytes,
  SHA-256 `15902895487c084c8f574666c31e7fa399d05b831fd61c1e3c3eee69d6576924`.
- [Detached signature](https://rufus.ie/files/grub-2.12/core.img.sig): 256 bytes,
  SHA-256 `ff13d50cdd305cbd7fd1b77fea0032e5745427311850b854b9833bd9cc107c3b`.

The signature verifies with SHA-256/RSA PKCS#1 v1.5 and the existing Rufus
download public key extracted from `src/pki.c`. `provenance.json` records both
URLs, response URLs, lengths and hashes. This is the server's binary/signature
pair, with no fixture signing key or production trust override. The test runs
offline; wrong-key signatures use a disposable test-only key and are rejected.

These bytes qualify signature-format/key compatibility only. They are not
installed, executed, or paired with ISO modules by this test. GRUB payload
construction is separately documented in `res/grub2/readme.txt`; that file
describes the current embedded release, not the provenance of this older core.
