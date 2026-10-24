# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only native Syslinux eligibility through the retained ISO/UDF view."""
import argparse
import errno
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from verify import ROOT, digest
sys.path.insert(0, str(ROOT / "linux/tests"))
from test_syslinux_scan import banner, make_image
from test_udf_images import make_image as make_udf, rename_udf_entry


def check_source(probe, image, error=0, mode="normal", problem="", filesystem=1, target="fat32"):
    before = digest(image)
    process = subprocess.run([probe, image, mode, target], capture_output=True, text=True, timeout=20)
    assert process.returncode == 0, (process.returncode, process.stdout, process.stderr)
    result = json.loads(process.stdout)
    assert result["error"] == error, (image.name, mode, result, process.stderr)
    assert result["fds"] == 0, result
    assert bool(result["problem"]) == bool(error), result
    assert problem in result["problem"], result
    assert result["filesystem"] == filesystem, result
    if mode == "replace-path":
        assert image.read_bytes() == b"replacement"
        assert digest(Path(str(image) + ".held")) == before
    else:
        assert digest(image) == before, "Read-only eligibility changed source"
    print(f"PASS source {image.name} {mode}: {result}", flush=True)
    return result


def fixtures(upstream):
    module = (upstream / "bios/com32/elflink/ldlinux/ldlinux.c32").read_bytes()
    loader = (upstream / "bios/core/isolinux.bin").read_bytes()
    config = b"DEFAULT checkpoint\n"
    nested = {"isolinux/isolinux.cfg": config, "isolinux/isolinux.bin": loader, "isolinux/ldlinux.c32": module}
    root = {"syslinux.cfg": config, "isolinux.bin": loader, "ldlinux.c32": module}
    suse = {"boot/x86_64/loader/" + name: data for name, data in
            (("isolinux.cfg", config), ("isolinux.bin", loader), ("ldlinux.c32", module))}
    suse["syslinux.cfg"] = b"DEFAULT wrong-root\n"
    bad = bytes(len(module))
    return (
        ("nested", nested, 0, ""), ("root", root, 0, ""), ("override", suse, 0, ""),
        ("unknown", {**nested, "isolinux/isolinux.bin": bytes(512)}, 0, ""),
        ("different-known-version", {**nested, "isolinux/isolinux.bin": banner("6.04 6.04-pre3")},
         errno.ENOTSUP, "does not match"),
        ("unknown-wrong-root", {**nested, "isolinux/isolinux.bin": bytes(512), "ldlinux.c32": bad},
         errno.ENOTSUP, "does not match"),
        ("mixed-case", {name.upper(): data for name, data in nested.items()}, 0, ""),
        ("efi-config-copy", {name.replace("isolinux/", "EFI/BOOT/"): data for name, data in nested.items()}, 0, ""),
        ("matched-root", {**nested, "LDLINUX.C32": module}, 0, ""),
        ("wrong-root", {**nested, "ldlinux.c32": bad}, errno.ENOTSUP, "does not match"),
        ("preserved-selected", {**nested, "isolinux/ldlinux.c32": bad}, 0, ""),
        ("unknown-wrong-selected", {**nested, "isolinux/isolinux.bin": bytes(512),
                                    "isolinux/ldlinux.c32": bad}, errno.ENOTSUP, "does not match"),
        ("short-preserved-module", {**nested, "isolinux/ldlinux.c32": module[:-1]}, 0, ""),
        ("unknown-short-module", {**nested, "isolinux/isolinux.bin": bytes(512),
                                  "isolinux/ldlinux.c32": module[:-1]}, errno.ENOTSUP, "size"),
        ("large-module", {**nested, "isolinux/isolinux.bin": bytes(512), "isolinux/ldlinux.c32": module * 4}, errno.ENOTSUP, "size"),
        ("absent-module", {name: data for name, data in nested.items() if not name.endswith('.c32')}, errno.ENOTSUP, "companion"),
        ("module-elsewhere", {**root, "ldlinux.c32": module, "boot/x86_64/loader/isolinux.cfg": config}, errno.ENOTSUP, "companion"),
        ("selected-extlinux", {**nested, "isolinux/extlinux.conf": config}, errno.ENOTSUP, "lookup differs"),
        ("fallback-extlinux", {**nested, "boot/syslinux/extlinux.conf": config}, errno.ENOTSUP, "lookup differs"),
        ("fallback2-extlinux", {**nested, "syslinux/extlinux.conf": config}, errno.ENOTSUP, "lookup differs"),
        ("root-extlinux-same", {**nested, "EXTLINUX.CONF": config, "ldlinux.c32": module}, 0, ""),
        ("root-extlinux-different", {**suse, "boot/x86_64/loader/syslinux.cfg": config, "extlinux.conf": config}, errno.ENOTSUP, "lookup differs"),
        ("fallback-ignored", {"isolinux/syslinux.cfg": config, "isolinux/isolinux.bin": loader,
                              "isolinux/ldlinux.c32": module, "boot/syslinux/extlinux.conf": config}, 0, ""),
        ("root-fallback-different", {**root, "boot/syslinux/extlinux.conf": config}, errno.ENOTSUP, "lookup differs"),
        ("root-extlinux", {**root, "extlinux.conf": config}, 0, ""),
        ("override-competing", {**suse, "boot/x86_64/loader/syslinux.cfg": config}, errno.ENOTSUP, "lookup differs"),
        ("backup-conflict", {**suse, "SYSLINUX.ORG": b"keep backup"}, errno.EEXIST, "backup"),
        ("unused-backup", {**root, "syslinux.org": b"keep backup"}, 0, ""),
        ("config-case-collision", {**root, "SYSLINUX.CFG": config}, errno.EEXIST, "collide"),
        ("module-case-collision", {**nested, "isolinux/LDLINUX.C32": module}, errno.EEXIST, "collide"),
        ("root-dot-alias", {**nested, "ldlinux.c32.": bad}, errno.ENOTSUP, "ordinary boot path"),
        ("root-space-alias", {**nested, "ldlinux.c32 ": bad}, errno.ENOTSUP, "ordinary boot path"),
        ("directory-dot-alias", {name.replace("isolinux/", "isolinux./"): data for name, data in nested.items()}, errno.ENOTSUP, "redirect"),
        ("parent-case-collision", {**nested, "ISOLINUX/other.txt": b"alias"}, errno.EEXIST, "collide"),
        ("sanitized-directory", {name.replace("isolinux/", "isolin*x/"): data for name, data in nested.items()}, errno.ENOTSUP, "redirect"),
        ("space-directory", {name.replace("isolinux/", "iso linux/"): data for name, data in nested.items()}, errno.ENOTSUP, "redirect"),
        ("module-directory", {**{name: data for name, data in nested.items() if not name.endswith('.c32')},
                              "isolinux/ldlinux.c32/keep": b"directory"}, errno.ENOTSUP, "companion"),
        ("root-loader-directory", {**nested, "ldlinux.sys/keep": b"directory"}, errno.ENOTSUP, "reserved"),
        ("root-config-directory", {**nested, "syslinux.cfg/keep": b"directory"}, errno.ENOTSUP, "reserved"),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("upstream", type=Path)
    parser.add_argument("--mkisofs", required=True)
    parser.add_argument("--target", choices=("fat32", "ntfs"), default="fat32")
    args = parser.parse_args()
    cases = fixtures(args.upstream)
    layouts = {name: entries for name, entries, error, problem in cases}
    with tempfile.TemporaryDirectory(prefix="rufus-syslinux-source-") as temporary:
        root = Path(temporary)
        for name, entries, error, problem in cases:
            image = make_image(root, name, entries, "bsdtar")
            result = check_source(args.probe, image, error, problem=problem, target=args.target)
            if name == "unknown":
                assert result["detected"] == 0 and result["inferred"] and result["selected"] == 0x0604
            if name == "large-module":
                assert result["reads"] == 0, "Oversized module was read before its size check"
            image.unlink()
            shutil.rmtree(root / name)
        image = make_image(root, "faults", {**layouts["nested"], "isolinux/isolinux.bin": bytes(512)}, "bsdtar")
        for mode, error, expected_fs in (
            ("cancel", errno.ECANCELED, 1), ("cancel-walk", errno.ECANCELED, 1),
            ("cancel-read", errno.ECANCELED, 1), ("read-fail", errno.EIO, 1),
            ("incomplete", errno.ENOTSUP, 1), ("wrong-view", errno.EINVAL, 0),
            ("missing-config", errno.ENOTSUP, 1), ("replace-path", errno.ESTALE, 1),
        ):
            check_source(args.probe, image, error, mode, filesystem=expected_fs, target=args.target)
        image = make_image(root, "symlink", layouts["nested"], "bsdtar")
        module = root / "symlink/isolinux/ldlinux.c32"
        payload = module.read_bytes()
        module.unlink()
        module.symlink_to("../payload.c32")
        (root / "symlink/payload.c32").write_bytes(payload)
        subprocess.run(["bsdtar", "--format", "iso9660", "-cf", image,
                        "-C", root / "symlink", "."], check=True, capture_output=True, timeout=10)
        check_source(args.probe, image, errno.ENOTSUP, problem="ordinary boot path", target=args.target)
        # Read the UDF view, including case-sensitive original paths. Renaming
        # only its companion must not be repaired from the untouched ISO view.
        image = make_udf(root, "UDF_SOURCE", layouts["mixed-case"], args.mkisofs)
        check_source(args.probe, image, filesystem=2, target=args.target)
        rename_udf_entry(image, "LDLINUX.C32", "MISSING.C32")
        check_source(args.probe, image, errno.ENOTSUP, problem="companion", filesystem=2, target=args.target)
    print(f"{len(cases)} source layouts, 8 fault cases, a symlink and 2 UDF-view checks passed; fixtures removed.", flush=True)


if __name__ == "__main__":
    main()
