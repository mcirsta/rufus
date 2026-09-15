# Shared Syslinux scan policy

Build the standalone C policy tests without Qt, libcdio or a Syslinux installer:

```sh
cmake -S tests/syslinux_scan -B /tmp/rufus-syslinux-scan-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/rufus-syslinux-scan-build
ctest --test-dir /tmp/rufus-syslinux-scan-build --output-on-failure
```

The 29 cases cover shortest/latest-tie config selection, openSUSE overrides,
case preservation, the config path limit, stale-field clearing, loader conflicts,
unknown candidates, suffix copying/truncation, family warnings, detected versus
inferred choices and unavailable fallback versions. Tests retain Rufus's prefix
comparison, including a root config's zero-length prefix and directory-name
prefix matches. A failed read must be skipped by the caller; a successfully read
unknown loader is a candidate and can replace an earlier known version under
the original conflict rule. These are intentionally different cases.

The report now distinguishes:

| Field | Meaning |
| --- | --- |
| `has_syslinux` | Configuration presence, independent of version recognition. `HAS_SYSLINUX` uses this field. |
| `sl_detected_version` / `sl_detected_version_ext` | Version/suffix associated with the chosen image loader. Zero remains unknown. |
| `sl_version` / `sl_version_str` / `sl_version_ext` | Existing fields for the selected installation version. |
| `sl_version_inferred` | A nonzero selected version came from Rufus's supplied v4/v6 fallback. |

The fallback retains the original empty suffix, rather than inventing an image
build suffix. A caller supplies the known fallback versions; zeros explicitly
mean unavailable. Selecting a version does not prove a Linux executable can
install the appropriate payload. That check remains required before writing.

The main Linux test runs the real shared traversal, content reader, version
parser and policy together on 17 generated ISO files:

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_syslinux_scan' --output-on-failure
```

Fixtures include Rufus's bundled v4/v6 loader bytes as data, config-only images,
loader/module-only images, conflicting versions/suffixes, preferred unknown
versus unreadable loaders, mixed-case paths and config-length boundaries.
Fallback-unavailable variants and an ordinary invalid-image case also run.
Tests create only disposable regular files and never install or boot a loader.

The original selection block in a temporary Linux harness passed the matching
16 supported-path images. The harness copies pre-fallback fields into the new
detection fields for comparison, without changing the original selection rules.
Unavailable fallback and overlong-only config cases exercise deliberate new
contracts and were excluded from that comparison. Native Windows validation
remains deferred; no broad all-input or boot-compatibility claim is made.

If every config exceeds the existing 127-byte report limit, selection fails
with presence still recorded and an empty config path. Windows reports a scan
error and Linux's fixture driver fails. This replaces the old loop's placeholder
path; it does not add long-path extraction support.
