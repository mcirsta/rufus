# SPDX-License-Identifier: GPL-3.0-or-later
"""File-only native Syslinux adapter: preflight, corruption and lifecycle checks."""
import argparse
import errno
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

from test_command import tools_directory
from test_source import fixtures
from verify import MIB, check_files, check_volume, digest, run
from test_syslinux_scan import make_image
from test_iso_extract import extract


def install(probe, installer, iso, image, tools, directory, env, mode="normal", offset=0,
            size=64 * MIB, cluster=512, error=0, started=True, fat_bits=32, major=6):
    source_hash = digest(iso) if iso is not None else None
    target_hash = digest(image) if not started else None
    poisoned = env.copy()
    poisoned.update({"PATH": "/nonexistent/rufus-poison", "MTOOLSRC": "/nonexistent/rufus-poison",
                     "LD_PRELOAD": "", "PYTHONPATH": "/nonexistent/rufus-poison"})
    source = iso if iso is not None else ("--standalone4" if major == 4 else "--standalone")
    output = run([probe, source, image, installer, tools, directory, mode, str(offset), str(size), str(cluster), str(fat_bits)], poisoned)
    result = json.loads(output)
    assert result["error"] == error and result["started"] == started, result
    assert bool(result["problem"]) == bool(error), result
    assert result["syncs"] == int(started), result
    assert result["sync_error"] == (errno.EIO if mode == "sync-fail" else 0), result
    assert result["fds"] == 0 and result["flags"] and result["position"] == 17, result
    assert result["facts"] and result["child"] == -1 and result["child_error"] == errno.ECHILD, result
    remaining = list(directory.glob("rufus-syslinux-*"))
    if mode == "cleanup-fail":
        assert len(remaining) == 1 and str(remaining[0]) in result["problem"], result
        (remaining[0] / "blocked").chmod(0o700)
        shutil.rmtree(remaining[0])
    else:
        assert not remaining, "Adapter left its private directory"
    if iso is not None:
        assert digest(iso) == source_hash, "Adapter changed source contents"
    if not started:
        assert digest(image) == target_hash, "Rejected request changed target"
    return result


def make_target(directory, tree, env, offset=0, cluster=512, fat_bits=32, label="RUFUS_TEST"):
    size = max(16 * MIB, cluster * 8192) if fat_bits == 16 else 64 * MIB * (cluster // 512)
    image = directory / "target ș '$.img"
    guard = b"guard-after-volume" * 32
    with image.open("wb") as stream:
        stream.truncate(offset + size)
        if offset:
            stream.write(b"guard-before-volume")
    run(["mkfs.fat", "-F", str(fat_bits), "-S", "512", "-s", str(cluster // 512),
         "-n", label, "-i", "1234abcd", "-h", str(offset // 512),
         f"--offset={offset // 512}", image, str(size // 1024)], env)
    with image.open("r+b") as stream:
        prefix = stream.read(offset)
        boot = stream.read(512)
        stream.seek(offset + size)
        stream.write(guard)
    if tree is not None:
        run(["mcopy", "-s", "-i", f"{image}@@{offset}", *sorted(tree.iterdir()), "::/"], env)
    return image, size, prefix, guard, boot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("installer", type=Path)
    parser.add_argument("upstream", type=Path)
    parser.add_argument("main_build", type=Path)
    parser.add_argument("--fat-bits", type=int, choices=(16, 32), default=32)
    parser.add_argument("--standalone", action="store_true")
    parser.add_argument("--major", type=int, choices=(4, 6), default=6)
    parser.add_argument("--other-installer", type=Path, help="Exercise rejection after a wrong-version install")
    args = parser.parse_args()
    if args.major == 4 and not args.standalone:
        parser.error("Only standalone v4 is qualified")
    with tempfile.TemporaryDirectory(prefix="rufus-syslinux-install-") as temporary:
        root = Path(temporary)
        env = os.environ.copy()
        env.update({"LC_ALL": "C", "MTOOLSRC": "/dev/null", "TMPDIR": str(root)})
        tools = root / "tools"
        tools_directory(tools)
        iso, tree = None, None
        if not args.standalone:
            entries = fixtures(args.upstream)[0][1]
            iso = make_image(root, "source", entries, "bsdtar")
            tree = root / "extracted"
            tree.mkdir()
            extract(args.main_build / "iso_extract_probe", iso, tree)
        cases = (
            ("normal", 0, True), ("offset", 0, True), ("cluster", 0, True),
            ("read-only", errno.EINVAL, False), ("append", errno.EINVAL, False),
            ("wrong-geometry", errno.EBADMSG if args.fat_bits == 16 else errno.ENOTSUP, False),
            ("wrong-filesystem", errno.ENOTSUP, False), ("unsupported-filesystem", errno.ENOTSUP, False), ("wrong-volume-id", errno.EBADMSG, False),
            ("incomplete-source", errno.ENOTSUP, False),
            ("cancel-before", errno.ECANCELED, False), ("no-subreaper", errno.EINVAL, False),
            ("missing-installer", errno.ENOENT, False), ("relative-installer", errno.EINVAL, False),
            ("existing-loader", errno.EEXIST, False),
            ("wrong-root-module", errno.EEXIST if args.standalone else errno.ENOTSUP, False),
            ("boot-code", errno.EBADMSG, True), ("boot-bpb", errno.EBADMSG, True),
            ("backup", errno.EBADMSG, True), ("loader-bytes", errno.EBADMSG, True),
            ("loader-size", errno.EBADMSG, True), ("loader-directory", errno.EBADMSG, True),
            ("loader-cycle", errno.EBADMSG, True), ("loader-short", errno.EBADMSG, True),
            ("module-bytes", errno.ENOTSUP, True), ("source-changed", errno.ESTALE, True),
            ("target-size", errno.ESTALE, True), ("sync-fail", errno.EIO, True),
            ("read-fail", errno.EIO, True), ("cancel-after", errno.ECANCELED, True),
            ("no-op", errno.ENOENT, True), ("tool-failure", errno.EIO, True),
            ("cancel-native", errno.ECANCELED, True), ("cleanup-fail", errno.EIO, True),
            ("unsupported-selection", errno.ENOTSUP, False), ("invalid-selection", errno.ENOTSUP, False),
            ("wrong-source-mode", errno.EINVAL, False), ("source-image", errno.EINVAL, False),
            ("source-path", errno.EINVAL, False), ("source-reader", errno.EINVAL, False),
            ("source-analysis", errno.EINVAL, False),
            ("unaligned-offset", errno.ENOTSUP, False), ("large-offset", errno.ENOTSUP, False),
        )
        if args.standalone:
            cases = tuple(case for case in cases if case[0] not in ("incomplete-source", "source-changed"))
            cases += (("existing-config", errno.EEXIST, False), ("existing-directory", errno.EEXIST, False),
                      ("existing-long-name", errno.EEXIST, False), ("deleted-file", errno.EEXIST, False),
                      ("no-label", 0, True))
        else:
            cases += (("image-version4", errno.ENOTSUP, False),)
        if args.major == 4:
            cases = tuple(case for case in cases if case[0] != "module-bytes")
        if args.other_installer:
            cases += (("wrong-provider", errno.EBADMSG, True),)
        if args.fat_bits == 16:
            cases = tuple(case for case in cases if case[0] != "backup")
            cases += (("max-cluster", 0, True),)
        for mode, error, started in cases:
            directory = root / mode
            directory.mkdir()
            offset = MIB if mode == "offset" else 0
            cluster = 65536 if mode == "max-cluster" else (1024 if mode == "cluster" else 512)
            label = "NO NAME" if mode == "no-label" else "RUFUS_TEST"
            image, size, prefix, guard, before = make_target(directory, tree, env, offset, cluster, args.fat_bits, label)
            program = args.installer
            selected_tools = tools
            if mode == "wrong-provider":
                program = args.other_installer
            elif mode == "missing-installer":
                program = root / "absent"
            elif mode == "relative-installer":
                program = Path("syslinux")
            elif mode == "no-op":
                program = Path("/usr/bin/true")
            elif mode == "tool-failure":
                program = Path("/usr/bin/false")
            elif mode in ("existing-loader", "wrong-root-module"):
                conflict = directory / "conflict"
                conflict.write_bytes(bytes(122656))
                name = "ldlinux.sys" if mode == "existing-loader" else "ldlinux.c32"
                run(["mcopy", "-i", image, conflict, f"::/{name}"], env)
            elif mode in ("existing-config", "existing-long-name", "deleted-file"):
                conflict = directory / "conflict"
                conflict.write_text("Existing content must not be overwritten.\n")
                name = "existing long file name.txt" if mode == "existing-long-name" else "syslinux.cfg"
                run(["mcopy", "-i", image, conflict, f"::/{name}"], env)
                if mode == "deleted-file":
                    run(["mdel", "-i", image, f"::/{name}"], env)
            elif mode == "existing-directory":
                run(["mmd", "-i", image, "::/existing"], env)
            elif mode == "cancel-native":
                selected_tools = directory / "tools"
                tools_directory(selected_tools)
                (selected_tools / "mcopy").unlink()
                stalled = selected_tools / "mcopy"
                stalled.write_text(f"#!{sys.executable}\nimport os, pathlib, time\n"
                                   "pathlib.Path(os.environ['TMPDIR'], 'ready').touch()\n"
                                   "time.sleep(60)\n")
                stalled.chmod(0o700)
            install(args.probe, program, iso, image, selected_tools, directory, env, mode, offset, size, cluster, error, started, args.fat_bits, args.major)
            if error == 0:
                if args.major == 6:
                    check_files(f"{image}@@{offset}", directory, env,
                                {"/ldlinux.c32": args.upstream / "bios/com32/elflink/ldlinux/ldlinux.c32"})
                check_volume(image, directory, offset, size, before, env, args.fat_bits)
            if mode != "target-size":
                with image.open("rb") as stream:
                    assert stream.read(offset) == prefix
                    stream.seek(offset + size)
                    assert stream.read() == guard
            print(f"PASS adapter {mode}: error={error}, started={started}", flush=True)
            shutil.rmtree(directory)
    kind = "standalone" if args.standalone else "image"
    print(f"{len(cases)} v{args.major} FAT{args.fat_bits} {kind} adapter cases passed; disposable files removed.", flush=True)


if __name__ == "__main__":
    main()
