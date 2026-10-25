# Retained CP35 independent FFU reader oracle

`Program.cs` and `Oracle.csproj` preserve the original CP35 qualification sources
without changing their algorithm or project references. They were previously
kept only in the workspace's `.rufus-test-artifacts/cp35/ffu-oracle/` directory.
The retained receipts are `cp34/oracles.json` and `cp35/checks.json` under that
workspace evidence directory.

## Upstream reader and build adjustment

The independent reader is [MobileTooling/img2ffu](https://github.com/MobileTooling/img2ffu)
at commit `d0dda2b2e138dd0412018bc733e64d1a92c385b0`.
`img2ffu-net10.patch` preserves the exact local changes to three upstream project
files: retarget `net8.0` to the installed `net10.0` SDK and remove their UTF-8
byte-order marks. The patch does not change the upstream FFU format code.
Apply it to a separate checkout of that pinned revision when reproducing the
original dependency build. The upstream MIT license is retained in this
repository as [LICENSE.img2ffu](../../../packaging/LICENSE.img2ffu).

The original project reference is deliberately preserved:

```text
../../cp34/img2ffu/Img2Ffu.Library.dll
```

It resolved from the original `cp35/ffu-oracle/` directory to the separately
built dependency in `cp34/img2ffu/`. That relative assembly layout is not supplied
by this repository directory. To reproduce the oracle elsewhere, recreate the
original layout or point the project reference at an independently built
`Img2Ffu.Library.dll` from the pinned upstream revision. Upstream assemblies,
NuGet caches and generated `bin`/`obj` files are not preserved here.

## Qualification boundary

The program accepts an FFU image path as its first argument. It uses the
independent reader to verify the image's hashes, decode store zero, and report
the original byte count, sector size and SHA-256 in JSON. Its reads are bounded
to the manifest's original disk size because the pinned upstream stream reports
an extra zero block and can return the requested count past EOF. The independent
descriptor and payload decoder remains unchanged.

This is a test-only qualification oracle. It is not linked into or installed
with the Linux application, and it does not claim coverage of every FFU layout.
Preserving these files during cleanup does not constitute a new build or test
run. No Microsoft binaries, image contents, credentials or signing keys are
included in this directory.
