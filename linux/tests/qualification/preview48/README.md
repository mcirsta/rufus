# Preview48 release qualification harness

This is the authored CP79 developer harness preserved from the delivery of
`linux-preview.48` at `a09b9d7d8c365b6c6ccce87763c172c028a5aeaa`.
The scripts below are byte-for-byte snapshots, including the corrected
per-phase GUI readiness wait used for the real Preview47 to Preview48 upgrade.
They are not a generic release framework or a newly executed qualification run.

## Fixed paths and prerequisites

The historical workspace is `/home/marius/proj/lufus`, with the application
checkout in `rufus/` and the working harness under `.rufus-test-artifacts/cp79/`.
These scripts intentionally retain their original paths, version assertions,
container names, immutable tag/commit and output names. Review those assumptions
before reuse. Replaying publication commands against an existing release is
not part of ordinary verification.

For the original layout, copy this directory's Python scripts to the CP79
working directory and its `distributions/` scripts to the matching subdirectory.
The code does not infer a different workspace from its installed location.
Required inputs include a matching Release CMake tree at
`.rufus-test-artifacts/cp32/release`, verified wimlib 1.14.5 and Syslinux
6.04-pre1/4.07 source trees under `.rufus-test-artifacts/deps/`, and the frozen
source/package receipts used by the publication checks. The tracked AUR recipe
and provider guides record the upstream URLs and SHA256 values.

Native package tests use rootless Podman with the tracked
`linux/packaging/Containerfile.debian` and `Containerfile.fedora`, a private CP79
store, and synthetic UID1000 `rufustest` inside disposable containers. Their
`pinned-base-images.json` input was:

- Debian: `docker.io/library/debian@sha256:15a71cc87173e191d5046f9c4288db4e8954b67f92e48d9587087826b0afe562`
- Fedora: `registry.fedoraproject.org/fedora@sha256:0d2a354dfdf7ba75083fe371eb6239842fe13f38c52d339dae1ebcf13c7c4062`

Create the private working/runtime directories before preparing images.
The real published Preview47 DEB/RPM are staged in each family's `previous/`
directory; their checksums are available in the Preview47 release manifest.
No host package installation, physical device write or host-service change is
part of this harness. GitHub publication uses the operator's existing `gh`
authentication; no credential, key or authentication configuration is included.

## Main delivery flow

Commands were run from the workspace using the staged CP79 paths:

```sh
python3 .rufus-test-artifacts/cp79/run-packages.py
python3 .rufus-test-artifacts/cp79/distributions/prepare-images.py
python3 .rufus-test-artifacts/cp79/distributions/run-native.py build debian
python3 .rufus-test-artifacts/cp79/distributions/run-native.py build fedora
python3 .rufus-test-artifacts/cp79/distributions/inspect-payloads.py qualified
python3 .rufus-test-artifacts/cp79/distributions/run-native.py qualify debian
python3 .rufus-test-artifacts/cp79/distributions/run-native.py qualify fedora
python3 .rufus-test-artifacts/cp79/distributions/inspect-payloads.py final
python3 .rufus-test-artifacts/cp79/distributions/finalize-delivery.py
```

`run-packages.py` builds/packages the Arch/CachyOS and generic artifacts and
calls `check-packages.py`. Native builds use Ninja's default parallelism and
`BUILD_TESTING=OFF`; their separate runtime containers check fresh installation,
real Preview47 to Preview48 upgrade, five executable/helper paths, provider availability,
locale/settings preservation, instance refusal and removal. The optional
`restage` mode is retained, but CP79 required no restaging after source freeze.

After the source tag was public, the actual GitHub archive was compared with
`git archive` and its own compressed-byte checksum was used by the canonical
`linux/packaging/aur/prepare.py`. CP79's AUR work was performed through that
tracked generator and direct verification commands; it produced no additional
Python/shell source to duplicate here. `PKGBUILD`, `.SRCINFO`, the source archive
and the deterministic submission bundle remain release outputs, not source
inputs for this directory. AUR submission is manual.

`prepare-publication.py` consumes the frozen source/AUR/package proof receipts
and writes release notes, qualification metadata, the package manifest and
checksums. `publish-draft.py` uploads the nine assets as a prerelease draft.
After saving the authenticated draft JSON as `draft-release.json`,
`verify-draft.py` checks every server-side asset digest/size and writes the
publish-request payload. Publishing that payload is a separate explicit action.
These command-line snapshots have top-level side effects. Review them before
running or importing them; publishing uses the operator's explicit authorization.
The one-off cleanup scripts are omitted.

After publication, an anonymous release listing was saved as
`public-release-listing.json`. `run-public-handoff.py` also consumes a captured
`ninja -t commands test_application_updates` listing and the existing normal
build libraries. It makes a private test copy that changes exactly the two
opt-in expected Preview47 version/page constants to Preview48, then runs only
the published-release parser/channel/version/page-button slot. It does not
modify production sources, open a browser or rerun the full Qt suite.

## Evidence boundary

The green Ubuntu 22.04/24.04 CI run 40087395293 and focused normal/sanitizer repair
checks were reused. Earlier qualified media, boot, geometry and desktop results
were retained with their original limits. These scripts do not establish full
Windows parity, physical USB/4Kn qualification, new AUR source-build coverage or
an additional full regression run. Historical results, failures, receipt JSON,
logs, screenshots and source hashes belong to the retained CP79 evidence, not
to this source-only snapshot. Current publication artifacts are available from
<https://github.com/mcirsta/rufus/releases/tag/linux-preview.48>.

The old `prepare-distributions.py` is omitted: it was a one-time CP78 to CP79
migration which would overwrite the final corrected harness. The two one-off
container cleanup/recovery scripts are also omitted. Generated test copies,
compiled files, copied packages, JSON/log outputs, dependency caches and research
material are not included.

## Original source inventory

Original paths are relative to `.rufus-test-artifacts/cp79/`. The hashes below
were checked against both the original files and these preserved copies.

| Path | Bytes | SHA256 |
| --- | ---: | --- |
| `check-packages.py` | 13577 | `d659ad52c5a969dbeeb17ed352fa28d4e31be363c43b1b00dfbcbcb70e08540a` |
| `prepare-publication.py` | 10451 | `ed505f7b9596e2fa92e17c48ae99d3694631353c6f515fe415e172f4d0f9b9a6` |
| `publish-draft.py` | 1103 | `b27ed55f4a1fc6aaebc06b67fbb9779994dba199f766f2f925c39bf8a2df657a` |
| `run-packages.py` | 1345 | `ca7af872f989fa3c06221e3c3e4f0ad98d9a4d5c7d7c362b4526216fb06282b0` |
| `run-public-handoff.py` | 4095 | `53a7154027986cefd5f15c8782e963c24071e0126ee8768f4f852094a9d2f8eb` |
| `verify-draft.py` | 1959 | `d46a8dedc7f0424577925b0123d8db668c9f6a7d3de4dd0788789f62ee563ee4` |
| `distributions/build-native.sh` | 851 | `0f52bfe7cc0a8afad557151861210ed784c23311f89dde45cd6c72fd7d2f22e0` |
| `distributions/finalize-delivery.py` | 3377 | `5fb3167efdff19238f88ed91fa2840c40dc491dfb797e66993f97cb77b3de2b3` |
| `distributions/inspect-installed.py` | 7115 | `008a22ddb518fe66788688e8916a95054df62d81362a94588064c98e41ebb944` |
| `distributions/inspect-payloads.py` | 2920 | `e0cf96e77e2597319d90de93a596aa4376383491a66565f2093596df1d367e21` |
| `distributions/prepare-images.py` | 1722 | `e095bd335746a1ac881e2228c1020a57fad1b06d3fde7d5b85ed277c13aaa7b3` |
| `distributions/qualify-native.sh` | 3765 | `8344fd38133ece6edd4b9838c3f6738515276a5840f0a573d851320e500f6477` |
| `distributions/restage-native.sh` | 533 | `9751a80fbba434bf29684fdf3909ace1ba6bf7d121409b2d843c949328a6ce78` |
| `distributions/run-native.py` | 1732 | `736538a1f0a6e4d87a718f0b3822fc65b38681fa54b998d6fc149ae2a09335f2` |
