# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot shared ISO extraction output using the qualified native Syslinux pair."""
import argparse
import errno
import os
from pathlib import Path
import shutil
import sys
import tempfile

from test_selection import select
from test_command import tools_directory
from test_install import install
from test_source import check_source
from verify import boot, check_files, check_volume, run, MIB, SECTOR_BYTES, ROOT

sys.path.insert(0, str(ROOT / "linux/tests"))
from test_iso_extract import extract
from test_syslinux_scan import make_image, scan
from test_mbr_image import invoke as partition_mbr, inspect_mbr
from test_fat_format import invoke as format_fat


def make_fixture(upstream, directory, config_path, root_exists, unknown, env, extra_config):
    fixture = directory / "fixture.bs"
    run(["nasm", "-f", "bin", ROOT / "linux/tests/vm/boot.asm", "-o", fixture], env)
    folder = str(Path(config_path).parent).strip("/")
    prefix = folder + "/" if folder else ""
    config = (b"SERIAL 0 115200\nUI menu.c32\nDEFAULT checkpoint\nPROMPT 0\nTIMEOUT 1\n"
              b"LABEL checkpoint\n  BOOT fixture.bs\n")
    loader = bytes(SECTOR_BYTES) if unknown else (upstream / "bios/core/isolinux.bin").read_bytes()
    entries = {config_path.lstrip("/"): config, prefix + "isolinux.bin": loader,
               prefix + "fixture.bs": fixture.read_bytes(), "keep.txt": b"Keep this ordinary file.\n",
               "ldlinux.sys": b"Image loader must be skipped, as on Windows."}
    for name, source in (
        ("ldlinux.c32", "elflink/ldlinux/ldlinux.c32"), ("menu.c32", "menu/menu.c32"),
        ("libcom32.c32", "lib/libcom32.c32"), ("libutil.c32", "libutil/libutil.c32"),
    ):
        entries[prefix + name] = (upstream / "bios/com32" / source).read_bytes()
    if root_exists and folder:
        entries["syslinux.cfg"] = b"DEFAULT wrong-root-config\n"
    if extra_config == "root-extlinux":
        entries["extlinux.conf"] = config
    elif extra_config == "unused-fallback":
        entries["boot/syslinux/extlinux.conf"] = b"DEFAULT wrong-fallback\n"
    image = make_image(directory, "source", entries, "bsdtar")
    return image, directory / "source", prefix


def qualify(args, directory, config_path, root_exists, unknown, env, extra_config, cluster=512):
    iso, tree, prefix = make_fixture(args.upstream, directory, config_path, root_exists, unknown, env, extra_config)
    loader = tree / (prefix + "isolinux.bin")
    module = tree / (prefix + "ldlinux.c32")
    selected = select(args.selection, loader, module, config_path, root_exists)
    detected = 0 if unknown else 0x0604
    suffix = "" if unknown else "/pre1"
    scan(args.main_build / "syslinux_scan_probe", iso, selected["config"], detected, suffix,
         selected=0x0604, inferred=unknown, overwrite=selected["backup"])
    check_source(args.main_build / "syslinux_source_probe", iso)
    target = directory / "extracted"
    target.mkdir()
    extract(args.main_build / "iso_extract_probe", iso, target)
    assert not (target / "ldlinux.sys").exists(), "Extractor copied the reserved loader"
    if selected["create"]:
        assert (target / "syslinux.cfg").read_text() == selected["text"]
    else:
        assert (target / "syslinux.cfg").read_bytes() == (tree / "syslinux.cfg").read_bytes()
    if selected["backup"]:
        assert (target / "syslinux.org").read_bytes() == (tree / "syslinux.cfg").read_bytes()
    image = directory / "fat.img"
    size = 64 * MIB * (cluster // SECTOR_BYTES)
    disk_size = size + MIB if args.mbr else size
    with image.open("wb") as stream:
        stream.truncate(disk_size)
    offset = 0
    if args.mbr:
        layout = partition_mbr(args.main_build / "mbr_image_probe", image, disk_size, cluster)
        offset, size = layout["OFFSET"], layout["SIZE"]
        inspect_mbr(image, disk_size, cluster)
        with image.open("r+b") as stream:
            stream.seek(SECTOR_BYTES)
            stream.write(b"P" * (offset - SECTOR_BYTES))
            stream.seek(offset + size)
            stream.write(b"T" * (disk_size - offset - size))
    with image.open("rb") as stream:
        prefix_bytes = stream.read(offset)
        stream.seek(offset + size)
        tail = stream.read()
    formatter = argparse.Namespace(probe=args.main_build / "fat_format_probe", mkfs=shutil.which("mkfs.fat"))
    format_fat(formatter, image, offset, size, cluster, label="RUFUS_TEST")
    with image.open("rb") as stream:
        stream.seek(offset)
        before = stream.read(SECTOR_BYTES)
    image_spec = f"{image}@@{offset}"
    run(["mcopy", "-s", "-i", image_spec, *sorted(target.iterdir()), "::/"], env)
    # Root install leaves every image module at its original path. The native
    # installer's additional root companion must match any root file it replaces.
    root_module = target / "ldlinux.c32"
    if root_module.exists():
        select(args.selection, loader, root_module, config_path, root_exists)
    tools = directory / "tools"
    tools_directory(tools)
    install(args.main_build / "syslinux_install_probe", args.installer, iso, image, tools, directory, env,
            offset=offset, size=size, cluster=cluster)
    files = {}
    for source in sorted(target.rglob("*")):
        if source.is_file():
            files["/" + source.relative_to(target).as_posix()] = source
    files["/ldlinux.c32"] = module
    check_files(image_spec, directory, env, files)
    run(["mcopy", "-i", image_spec, "::/ldlinux.sys", directory / "installed.sys"], env)
    check_volume(image, directory, offset, size, before, env)
    with image.open("rb") as stream:
        assert stream.read(offset) == prefix_bytes, "FAT creation/installation changed MBR or leading gap"
        stream.seek(offset + size)
        assert stream.read() == tail, "FAT creation/installation changed the trailing gap"
    if args.mbr:
        partition_mbr(args.main_build / "mbr_image_probe", image, disk_size, cluster, operation="check")
        inspect_mbr(image, disk_size, cluster)
    boot(image, env)
    print(f"PASS ISO -> shared selection/extraction -> native root install -> menu -> {config_path}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("installer", type=Path)
    parser.add_argument("selection", type=Path)
    parser.add_argument("upstream", type=Path)
    parser.add_argument("main_build", type=Path)
    parser.add_argument("--mbr", action="store_true", help="Qualify two partitioned whole-disk images")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-syslinux-layout-") as temporary:
        root = Path(temporary)
        env = os.environ.copy()
        env.update({"LC_ALL": "C", "MTOOLSRC": "/dev/null"})
        if args.mbr:
            for name, config, cluster in (("nested-mbr", "/isolinux/isolinux.cfg", 512),
                                          ("root-mbr", "/syslinux.cfg", 1024)):
                directory = root / name
                directory.mkdir()
                env["TMPDIR"] = str(directory)
                qualify(args, directory, config, config == "/syslinux.cfg", False, env, "", cluster)
                shutil.rmtree(directory)
            print("Two MBR -> FAT32 -> Syslinux -> menu boots passed; temporary disks removed.", flush=True)
            return
        for name, config, exists, unknown, extra_config in (
            ("nested", "/isolinux/isolinux.cfg", False, False, ""),
            ("unknown", "/isolinux/isolinux.cfg", False, True, ""),
            ("root", "/syslinux.cfg", True, False, ""),
            ("override", "/boot/x86_64/loader/isolinux.cfg", True, False, ""),
            ("nested-syslinux", "/isolinux/syslinux.cfg", False, False, ""),
            ("root-extlinux", "/syslinux.cfg", True, False, "root-extlinux"),
            ("unused-fallback", "/isolinux/syslinux.cfg", False, False, "unused-fallback"),
            ("efi-config-copy", "/EFI/BOOT/isolinux.cfg", False, False, ""),
        ):
            directory = root / name
            directory.mkdir()
            env["TMPDIR"] = str(directory)
            qualify(args, directory, config, exists, unknown, env, extra_config)
            shutil.rmtree(directory)
        # Root and selected-directory modules need independent checks. Reject a
        # conflicting root companion before making a disk or invoking the installer.
        mismatch = root / "root-module.c32"
        mismatch.write_bytes(bytes(122656))
        select(args.selection, args.upstream / "bios/core/isolinux.bin", mismatch,
               error=errno.ENOTSUP)
    print("Eight checked/extracted ISO layouts booted; temporary media removed.", flush=True)


if __name__ == "__main__":
    main()
