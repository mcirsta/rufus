# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage the private installer, check payload pins and boot its installed bytes."""
import argparse
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile

PROBES = Path(__file__).resolve().parents[1] / "probes/syslinux"
sys.path.insert(0, str(PROBES))
from verify import digest, run


def reject_changed_payloads(args, directory, env):
    source = directory / "source"
    payloads = ["core/ldlinux.bss", "core/ldlinux.sys"]
    if args.major == 6:
        payloads.append("com32/elflink/ldlinux/ldlinux.c32")
    prefix = Path("bios") if args.major == 6 else Path()
    version = "6.04-pre1" if args.major == 6 else "4.07"
    # Only configure up to the payload checks; no source compilation is needed
    # for these deliberately invalid inputs. Never edit the supplied release.
    for path in (prefix / "version.h", Path("libinstaller/bin2c.pl")):
        target = source / path
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.upstream / path, target)
    for payload in payloads:
        target = source / prefix / payload
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.upstream / prefix / payload, target)
    for index, payload in enumerate(payloads):
        target = source / prefix / payload
        original = target.read_bytes()
        changed = bytearray(original)
        changed[-1] ^= 1
        target.write_bytes(changed)
        source_option = "SYSLINUX_SOURCE_DIR" if args.major == 6 else "RUFUS_SYSLINUX4_UPSTREAM"
        command = [args.cmake, "-S", PROBES, "-B", directory / f"bad-{index}", "-G", "Ninja",
                   "-DBUILD_TESTING=OFF", f"-D{source_option}={source}"]
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
        output = result.stdout + result.stderr
        print(output, end="", flush=True)
        assert result.returncode != 0, f"Accepted changed {payload}"
        # CMake wraps long diagnostics, including the companion's long path.
        assert f"Payload does not match qualified {version}: {payload}" in " ".join(output.split()), output
        target.write_bytes(original)
        print(f"PASS rejected changed {payload}", flush=True)


def check_package(args, directory, env):
    stage = directory / "stage"
    prefix = Path("/opt/rufus package test")
    install_env = env.copy()
    install_env["DESTDIR"] = str(stage)
    name = "rufus-syslinux" if args.major == 6 else "rufus-syslinux4"
    component = "SyslinuxRuntime" if args.major == 6 else "Syslinux4Runtime"
    run([args.cmake, "--install", args.build, "--config", args.config,
         "--prefix", prefix, "--component", component], install_env)
    installer = stage / str(prefix / args.libexec / name).lstrip("/")
    notices = stage / str(prefix / args.datadir / "licenses" / name).lstrip("/")
    expected = {installer, notices / "COPYING", notices / "README.md"}
    extlinux = None
    if args.major == 6:
        assert args.extlinux is not None, "The v6 package includes the native NTFS provider"
        extlinux = installer.with_name("rufus-extlinux")
        expected.add(extlinux)
    actual = {path for path in stage.rglob("*") if not path.is_dir()}
    assert actual == expected, (actual, expected)
    for path in expected:
        assert not path.is_symlink(), path
        expected_mode = 0o755 if path in (installer, extlinux) else 0o644
        assert stat.S_IMODE(path.stat().st_mode) == expected_mode, path
    assert digest(installer) == digest(args.installer), "Installed executable differs from the build"
    assert (notices / "COPYING").read_bytes() == (args.upstream / "COPYING").read_bytes()
    assert (notices / "README.md").read_bytes() == Path(__file__).with_name("README.md").read_bytes()
    run([installer, "--version"], env)
    if extlinux:
        assert digest(extlinux) == digest(args.extlinux), "Installed NTFS provider differs from the build"
        run([extlinux, "--version"], env)
        print(f"PACKAGE extlinux SHA-256 {digest(extlinux)}", flush=True)
    print(f"PACKAGE executable SHA-256 {digest(installer)}", flush=True)
    # Boot the staged executable at a relocated path through the checked adapter.
    if args.major == 4:
        run([sys.executable, PROBES / "test_standalone.py", installer, args.upstream, args.build,
             "--major", "4", "--probe", args.probe], env, timeout=90)
    else:
        run([sys.executable, PROBES / "test_layout.py", installer, args.probe,
             args.upstream, args.build, "--mbr"], env, timeout=90)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cmake")
    parser.add_argument("build", type=Path)
    parser.add_argument("config")
    parser.add_argument("installer", type=Path)
    parser.add_argument("probe", type=Path)
    parser.add_argument("upstream", type=Path)
    parser.add_argument("libexec", type=Path)
    parser.add_argument("datadir", type=Path)
    parser.add_argument("--major", type=int, choices=(4, 6), default=6)
    parser.add_argument("--extlinux", type=Path)
    args = parser.parse_args()
    env = os.environ.copy()
    env["LC_ALL"] = "C"
    with tempfile.TemporaryDirectory(prefix="rufus-syslinux-package-") as temporary:
        directory = Path(temporary)
        check_package(args, directory, env)
        reject_changed_payloads(args, directory, env)
    print("Package checks, two whole-disk boots and payload rejections passed; fixtures removed.", flush=True)


if __name__ == "__main__":
    main()
