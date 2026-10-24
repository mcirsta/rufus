# Arch User Repository source package

This directory prepares the `rufus-linux-preview` AUR source-build recipe. It is
separate from `../PKGBUILD.in`, which packages an existing local CMake build and
cannot be submitted to the AUR.

The AUR recipe builds the unprivileged GUI, matching helper/polkit policy and
both native Syslinux installers from a fixed release. It uses the unchanged
Rufus resources and 38-language catalog. Linux-specific text may remain English.
It keeps UDF 1.02–2.01 support; UDF 2.50/2.60, ReFS formatting and Dev Drive
recognition are documented release gaps.

## Prepare the reviewed candidate

Supply the exact source archive from the release and the HTTPS URL where the
same bytes will be published. The archive must have one top-level directory
containing `linux/`, `src/`, `res/`, `docs/` and the original license. Generated
submission files must remain outside that archive to avoid a checksum cycle.
The script derives the version from its CMake and Windows-resource sources,
pins the archive's SHA-256, checks Bash syntax and generates `.SRCINFO` with
`makepkg`. It does not download, build, install or publish anything.

```sh
python linux/packaging/aur/prepare.py \
  --source-archive /absolute/path/to/the-release-source.tar.gz \
  --source-url https://github.com/mcirsta/rufus/archive/refs/tags/linux-preview.48.tar.gz \
  --output /absolute/path/to/aur-candidate
```

Set the actual maintainer name/contact in the generated `PKGBUILD`, or supply
`--maintainer 'Approved Name <approved contact>'`, before submission. Regenerate
`.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO` after changing any metadata.
The recipe itself is 0BSD licensed; the application retains its GPL license and
bundled third-party notices.

## Build on Arch

Review the generated `PKGBUILD`, then use an ordinary user with `base-devel`:

```sh
cd /absolute/path/to/aur-candidate
makepkg --verifysource
makepkg -s
```

`makepkg -s` asks the package manager to install missing build dependencies on
the builder's system. For clean-environment qualification use Arch `devtools`
instead. Ninja uses its default parallelism. No `.install` script is needed:
standard desktop/icon package hooks update caches. Do not run the GUI as root.
Install the resulting package using the normal package manager; building never
writes host disks or installs the application.

The required `libntfs-3g` dependency supplies the library and development headers
used by the extended WIM provider. NTFS utilities are optional: current Arch
splits `ntfsprogs` (format/resize tools), `libntfs-3g` and `ntfs-3g` (FUSE driver).
`ntfsprogs-plus` may provide preferred utilities on distributions that offer it.
The library backend is fixed to the declared Arch dependency at build time.
Formatter paths are fixed under `/usr/bin` even when those optional tools are
absent on the builder. Runtime preflight reports missing providers before writes.

Pinned corresponding sources for wimlib 1.14.5 and Syslinux 6.04-pre1/4.07 are
included in the recipe's HTTPS sources and checked with SHA-256. Their exact
release payloads are checked again by CMake. Source provenance is described in
[the WIM guide](../../../docs/linux-wimlib-prototype.md) and
[the Syslinux guide](../../syslinux/README.md). Upstream Syslinux's signed HTTPS
checksum lists were compared; the PGP signature was not authenticated.

## Submission boundary

The release owner will submit the package manually. The delivered candidate is
`PKGBUILD`, `.SRCINFO` and `LICENSE`; preparing these files creates no AUR entry.
A publicly downloadable matching port source release, an actual maintainer and
a recorded clean Arch build are required before upload. Set your maintainer
name/contact, verify the source URL from a fresh directory and recheck the
package name before submitting. Do not submit binary packages or downloaded
archives.

The planned release tag is `linux-preview.48` in `mcirsta/rufus`. Generate the
final candidate from the archive actually downloaded from GitHub after the tag
is public. A locally generated archive can qualify the preparation/build flow,
but its compressed-byte checksum must not be assumed to match GitHub's archive.

The official AUR RPC check on 2026-10-24 found no `rufus-linux-preview`, `rufus` or
`rufus-git` entry. `lufus` already belongs to an unrelated Python application.
Name availability is not a reservation.

References: [AUR submission guidelines](https://wiki.archlinux.org/title/AUR_submission_guidelines),
[Arch package guidelines](https://wiki.archlinux.org/title/Arch_package_guidelines),
[PKGBUILD manual](https://man.archlinux.org/man/PKGBUILD.5.en).
