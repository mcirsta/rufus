#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify MBR preparation or format/mount handoffs on disposable USB guests."""
import argparse
import errno
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

import prepare
from fat_extract import add_executable, digest, run, values
from test_mbr_prepare import DISK_BYTES, seed_target, inspect_cleared
from test_mbr_image import MIB, SECTOR_BYTES, extent, inspect_mbr, trailing_extent
from test_fat_format import copy_volume

HANDOFF_DISK_BYTES = 64 * MIB  # FAT32's track-rounded partition reaches the last sector.
EXT_DISK_BYTES = 320 * MIB  # Rufus's standalone ext layout starts at 256 MiB disks.
FAT16_CLUSTER_BYTES = 2048
GPT_BACKUP_BYTES = 33 * SECTOR_BYTES
NTFS_HELPER_TIMEOUT_SECONDS = 300  # Complete NTFS matrices, including sanitizer/TCG runs.
GRUB2_HELPER_TIMEOUT_SECONDS = 300  # 22 complete jobs plus partial-write cancellation.
COMPRESSED_HELPER_TIMEOUT_SECONDS = 600  # Seed jobs, full-stream preflight and independent range hashes on TCG.
XZ_HELPER_TIMEOUT_SECONDS = 1200  # 45 real jobs, including Debug/sanitized XZ full-image decode/hash passes.
XZ_QT_HELPER_TIMEOUT_SECONDS = 1800  # Both complete SELECT/controller jobs add ten large decode/hash passes.
BZIP2_HELPER_TIMEOUT_SECONDS = 2400  # 69 jobs: existing 47 plus BZIP2 checks, claims and four full-image passes.
BZIP2_QT_HELPER_TIMEOUT_SECONDS = 900  # Five seed, five raw and one complete 64-MiB ordinary BZIP2 controller job.
LZMA_HELPER_TIMEOUT_SECONDS = 1500  # Focused 38 jobs, including actual source faults and four full LZMA decode/hash passes.
LZMA_QT_HELPER_TIMEOUT_SECONDS = 1200  # Five seed/five raw and one full ordinary LZMA scan/controller/restore.
LZW_HELPER_TIMEOUT_SECONDS = 1800  # Focused LZW format/source/claim/data faults and four full restore decodes.
ZIP_HELPER_TIMEOUT_SECONDS = 1800  # Five entry methods, real claim/fault jobs and a full deflate restore.
ZSTD_HELPER_TIMEOUT_SECONDS = 1800  # Focused frames/faults plus a full poisoned Zstd restore.
VTSI_HELPER_TIMEOUT_SECONDS = 1200  # Real sparse claim/readback and one complete 64-MiB restore.
LZW_QT_HELPER_TIMEOUT_SECONDS = 2400  # Two full ordinary restores plus five real fallback source/data faults.
ZIP_QT_HELPER_TIMEOUT_SECONDS = 1200  # One full ordinary deflate SELECT/controller/restore and setup jobs.
ZSTD_QT_HELPER_TIMEOUT_SECONDS = 1200  # One full ordinary Zstd SELECT/controller/restore and setup jobs.
INITRAMFS_PACK_TIMEOUT_SECONDS = 60  # Qt/sanitizer archives contain hundreds of MiB.


def make_guest(args, base):
    root = base / "root"
    root.mkdir()
    (root / "lib").symlink_to("usr/lib")
    prepare.bootstrap(base, root, args.kernel)
    init = base / "initramfs"
    for binary in args.binaries:
        add_executable(init, binary)
    if args.handoff or args.install or args.helper:
        (init / "run/dbus").mkdir(parents=True, exist_ok=True)
        # A private real bus with fixture UDisks properties; no host service,
        # polkit or authorization bypass is built into the production backend.
        shutil.copytree("/usr/lib/locale/C.utf8", init / "usr/lib/locale/C.utf8")
        prepare.write(init, "etc/passwd", "root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run/caller-home:/bin/bash\n")
        prepare.write(init, "etc/group", "root:x:0:\n")
        prepare.write(init, "etc/nsswitch.conf", "passwd: files\ngroup: files\n")
        prepare.write(init, "etc/machine-id", "3f072f338dfb4029b8f2c40f6a5ac313\n")
        prepare.write(init, "etc/rufus-test-bus.conf", '''<busconfig>
  <type>system</type>
  <listen>unix:path=/run/dbus/system_bus_socket</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="root"/>
    <allow user="caller"/>
    <allow own="*"/>
    <allow send_destination="*"/>
    <allow receive_sender="*"/>
  </policy>
</busconfig>
''')
    if args.install or args.helper:
        shutil.copy2(args.iso, init / "source.iso")
        (init / "usr/bin/sh").symlink_to("bash")
        prepare.write(init, "etc/rufus-install-fixture", "install\n")
        prepare.write(init, "run/tools/mcopy", '''#!/bin/bash
set -eu
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
/usr/bin/test_mbr_handoff_guest --claim
exec /usr/bin/mcopy "$@"
''', 0o755)
        (init / "run/tools/mattrib").symlink_to("/usr/bin/mattrib")
    if args.helper:
        if args.alpine:
            prepare.write(init, "etc/rufus-alpine-fixture", "alpine\n")
        else:
            shutil.copy2(base / "unknown/source.iso", init / "unknown.iso")
            shutil.copy2(base / "incompatible/source.iso", init / "incompatible.iso")
        prepare.write(init, "etc/rufus-helper-fixture", "helper\n")
    if args.zip_supplement:
        from zip_supplement import stage_fixture
        stage_fixture(base, init)
        prepare.write(init, "etc/rufus-zip-supplement-fixture", "zip-supplement\n")
    if args.device_source:
        from device_source import stage
        stage(base, init)
        prepare.write(init, "etc/rufus-device-source-fixture", "device-source\n")
    if args.device_options:
        prepare.write(init, "etc/rufus-device-options-fixture", "device-options\n")
        prepare.write(init, "etc/rufus-format-helper-fixture", "format-helper\n")
    if args.ntfs_options:
        from ntfs_options import stage_fixture
        stage_fixture(args, base, init)
    if args.iso_resources:
        from iso_resources import stage_fixture
        stage_fixture(args,base,init)
    if args.legacy_iso:
        from legacy_iso import stage_fixture
        stage_fixture(args,base,init)
    if args.geometry_batch:
        from geometry_batch import stage_fixture
        stage_fixture(args, base, init)
    if args.iso_options:
        from iso_options import stage_fixture
        stage_fixture(args, base, init)
    if args.legacy_boot:
        from legacy_boot import stage_fixture
        stage_fixture(args,base,init)
    if args.runtime_validation:
        from runtime_validation import stage_fixture
        stage_fixture(args, base, init)
    if args.native_formats:
        import zipfile
        prepare.write(init, "etc/rufus-native-formats-fixture", "native-formats\n")
        if args.udf_indexing_batch:
            prepare.write(init, "etc/rufus-udf-indexing-fixture", "udf-indexing\n")
        prepare.write(init, "etc/rufus-format-helper-fixture", "format-helper\n")
        (init / "tmp").mkdir(mode=0o1777, exist_ok=True)
        (init / "tmp").chmod(0o1777)
        provider = init / str(args.mkudffs).lstrip('/')
        provider.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(init / 'usr/bin/mkudffs', provider)
        with zipfile.ZipFile(init / 'native-supplement.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('安装/native.txt', 'native UDF overlay\n')
    if args.windows_togo:
        from windows_togo import stage_fixture
        stage_fixture(args, base, init)
        prepare.write(init, "etc/rufus-format-helper-fixture", "format-helper\n")
        (init / "usr/bin/sh").symlink_to("bash")
        (init / "tmp").mkdir(mode=0o1777, exist_ok=True)
        (init / "tmp").chmod(0o1777)
    if args.helper or args.syslinux_major:
        # Install in the production compile-time location, never a path supplied
        # by the helper request or an environment override.
        destination = init / args.installer_path.lstrip("/")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(init / "usr/bin" / args.installer_name, destination)
    shutil.copytree("/usr/lib/gconv", init / "usr/lib/gconv")
    if args.sfd:
        prepare.write(init, "etc/rufus-sfd-fixture", "sfd\n")
    if args.exfat:
        prepare.write(init, "etc/rufus-exfat-fixture", "exfat\n")
    if args.exfat_helper or args.fat_helper or args.ntfs_helper or args.grub2_ext_helper:
        prepare.write(init, "etc/rufus-format-helper-fixture", "format-helper\n")
    if args.windows_togo or args.windows_bios or args.syslinux_major or args.freedos_helper or args.exfat_helper or args.fat_helper or args.ntfs_helper or args.grub2_ext_helper or args.mounted_iso_helper or args.mounted_raw_helper or args.zip_supplement or args.native_formats or args.runtime_validation or args.legacy_boot or args.iso_options or args.ntfs_options or args.iso_resources or args.device_options or args.legacy_iso or args.geometry_batch:
        # Guest-only authorization stand-in: no host polkit or host devices.
        # Hold the launcher after the real helper exits to audit client READY.
        prepare.write(init, "usr/bin/pkexec", '''#!/bin/bash
[[ $(< /sys/class/dmi/id/product_name) == RufusMBRFixture ]] || exit 77
[[ $1 == --disable-internal-agent ]] || exit 64
shift
echo launch >> /run/format-launches
export PKEXEC_UID=1000
if [[ -f /etc/rufus-grub2-image-fixture || -f /etc/rufus-winpe-gui-fixture || -f /etc/rufus-grub4dos-fixture || -f /etc/rufus-legacy-boot-fixture || -f /etc/rufus-iso-options-fixture || -f /etc/rufus-ntfs-options-fixture || -f /etc/rufus-iso-resources-fixture || -f /etc/rufus-legacy-iso-fixture ]]; then
    # The guest driver runs as PID 1's root child. Model caller ownership of
    # its private launcher snapshot before invoking the unchanged root helper.
    for directory in /tmp/rufus-grub2-* /tmp/rufus-boot-resources-*; do
        if [[ -f "$directory/grldr" ]]; then
            chown 1000:1000 "$directory" "$directory/grldr"
        elif [[ -f "$directory/diskcopy.dll" ]]; then
            chown 1000:1000 "$directory" "$directory/diskcopy.dll"
        elif [[ -f "$directory/syslinux.resources" ]]; then
            chown 1000:1000 "$directory" "$directory/syslinux.resources"
        elif [[ -f "$directory/core.img" ]]; then
            chown 1000:1000 "$directory" "$directory/core.img"
        fi
    done
fi
if [[ -f /etc/rufus-grub4dos-image-gui-fixture ]]; then
    "$@" | /usr/bin/tee /run/format-helper-output
    result=${PIPESTATUS[0]}
else
    "$@"
    result=$?
fi
echo held > /run/format-exit-held
if [[ -f /etc/rufus-device-source-fixture ]]; then
    for attempt in {1..1000}; do
        [[ -f /run/format-exit-release ]] && exit "$result"
        sleep .01
    done
    exit 78
fi
if [[ -f /etc/rufus-syslinux-fixture || -f /etc/rufus-grub2-fixture || -f /etc/rufus-windows-bios-fixture || -f /etc/rufus-grub4dos-fixture || -f /etc/rufus-grub4dos-image-fixture ]]; then
    for attempt in {1..1000}; do
        [[ -f /run/format-exit-release ]] && exit "$result"
        sleep .01
    done
    exit 78
fi
sleep .2
exit "$result"
''', 0o755)
    if args.ntfs_helper or args.uefi_exfat_helper or args.syslinux_uefi or args.grub_uefi or (args.windows_dual and not args.windows_fat32):
        (init / "tmp").mkdir(mode=0o1777)
        (init / "tmp").chmod(0o1777)
        shutil.copy2(args.boot_payload, init / "uefi-ntfs.img")
    if args.ntfs_helper:
        prepare.write(init, "etc/rufus-ntfs-helper-fixture", "ntfs-helper\n")
    if args.standalone_uefi:
        prepare.write(init, "etc/rufus-uefi-ntfs-helper-fixture", "standalone UEFI:NTFS\n")
    if args.mbr_fat_helper or args.mbr_ntfs_helper or args.mbr_exfat_helper or args.grub2_helper or args.grub2_ext_helper:
        prepare.write(init, "etc/rufus-nonboot-mbr-fixture", "mbr\n")
    if args.sfd_helper or args.sfd_ntfs_helper or args.sfd_exfat_helper:
        prepare.write(init, "etc/rufus-sfd-helper-fixture", "sfd-helper\n")
    if args.mounted_helper:
        prepare.write(init, "etc/rufus-mounted-helper-fixture", "mounted-helper\n")
    if args.mounted_raw_helper:
        prepare.write(init, "etc/rufus-mounted-raw-helper-fixture", "mounted-raw-helper\n")
    if args.compressed_raw_helper:
        prepare.write(init, "etc/rufus-compressed-helper-fixture", "compressed-helper\n")
    if args.xz_raw_helper:
        prepare.write(init, "etc/rufus-xz-helper-fixture", "xz-helper\n")
    if args.bzip2_raw_helper:
        prepare.write(init, "etc/rufus-bzip2-helper-fixture", "bzip2-helper\n")
    if args.lzma_raw_helper:
        prepare.write(init, "etc/rufus-lzma-helper-fixture", "lzma-helper\n")
    if args.zip_raw_helper:
        prepare.write(init, "etc/rufus-zip-helper-fixture", "zip-helper\n")
    if args.zstd_raw_helper:
        prepare.write(init, "etc/rufus-zstd-helper-fixture", "zstd-helper\n")
    if args.vtsi_raw_helper:
        prepare.write(init, "etc/rufus-vtsi-helper-fixture", "vtsi-helper\n")
    if args.vhd_raw_helper:
        prepare.write(init, "etc/rufus-vhd-helper-fixture", "vhd-helper\n")
    if args.save_read_only:
        prepare.write(init, "etc/rufus-save-read-only-fixture", "save-read-only\n")
    if args.save_qt_helper and args.save_export:
        prepare.write(init, "etc/rufus-save-export-fixture", "save-export\n")
        for module in ('udf', 'loop'):
            lines = subprocess.check_output(['modprobe', '--show-depends', '-S', args.kernel, module], text=True)
            for line in lines.splitlines():
                if line.startswith('insmod '):
                    source_module = Path(line.split()[1]).resolve()
                    destination = init / source_module.relative_to('/')
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source_module, destination)
        run(['depmod', '-b', str(init), args.kernel])
        encoder = init / str(args.export_encoder).lstrip('/')
        encoder.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(init / 'usr/bin/mkisofs', encoder)
    if args.save_qt_helper:
        prepare.write(init, "etc/rufus-save-qt-fixture", "save-qt\n")
        from test_image_save import gpt_image
        (init / "save-gpt.raw").write_bytes(gpt_image(size=64 * 1024 * 1024))
        prepare.write(init, "etc/rufus-compressed-qt-fixture", "compressed-qt\n")
    if args.media_appearance:
        prepare.write(init, "etc/rufus-media-appearance-fixture", "appearance\n")
        shutil.copy2(base / "appearance.iso", init / "appearance.iso")
        shutil.copy2(base / "expected-autorun.inf", init / "expected-autorun.inf")
    if args.bad_blocks:
        prepare.write(init, "etc/rufus-badblocks-fixture", "badblocks\n")
    if args.old_bios:
        prepare.write(init, "etc/rufus-old-bios-fixture", "old-bios\n")
    if args.full_format:
        prepare.write(init, "etc/rufus-full-format-fixture", "full-format\n")
    if args.zero_device:
        prepare.write(init, "etc/rufus-zero-device-fixture", "zero-device\n")
    if args.image_entry:
        prepare.write(init, "etc/rufus-image-entry-fixture", "image-entry\n")
    if args.ffu_partial:
        prepare.write(init, "etc/rufus-ffu-partial-fixture", "ffu-partial\n")
    if args.ffu_geometry:
        prepare.write(init, "etc/rufus-ffu-geometry-fixture", "ffu-geometry\n")
    if args.ffu_layouts:
        prepare.write(init, "etc/rufus-ffu-layouts-fixture", "ffu-layouts\n")
    if args.ffu_optimized:
        prepare.write(init, "etc/rufus-ffu-optimized-fixture", "ffu-optimized\n")
        shutil.copytree(args.ffu_optimized, init / "ffu-optimized")
    if args.ffu_stores:
        prepare.write(init, "etc/rufus-ffu-stores-fixture", "ffu-stores\n")
    if args.ffu_catalog:
        shutil.copy2(args.ffu_catalog, init / "etc/rufus-ffu-test.cat")
    if args.ffu_variants:
        prepare.write(init, "etc/rufus-ffu-variants-fixture", "ffu-variants\n")
    if args.ffu_qt_helper:
        prepare.write(init, "etc/rufus-ffu-qt-fixture", "ffu-qt\n")
        prepare.write(init, "etc/rufus-compressed-qt-fixture", "compressed-qt\n")
    if args.virtual_disk_chains:
        prepare.write(init, "etc/rufus-virtual-disk-fixture", "virtual-disk\n")
    if args.vhdx_qt_helper:
        prepare.write(init, "etc/rufus-vhdx-qt-fixture", "vhdx-qt\n")
        prepare.write(init, "etc/rufus-compressed-qt-fixture", "compressed-qt\n")
    if args.lzw_raw_helper or args.lzw_qt_helper:
        from lzw_fixtures import stage_lzw_fixtures
        if args.lzw_raw_helper:
            prepare.write(init, "etc/rufus-lzw-helper-fixture", "lzw-helper\n")
        if args.lzw_qt_helper:
            prepare.write(init, "etc/rufus-lzw-qt-fixture", "lzw-qt\n")
        shutil.copy2(init / 'usr/bin' / args.lzw_encoder.name, init / 'usr/bin/rufus-test-compress')
        fixture_manifest = stage_lzw_fixtures(init / 'lzw-fixtures', args.lzw_encoder)
        (base / 'lzw-fixtures.json').write_text(json.dumps(fixture_manifest, indent=2) + '\n')
        print(f"LZW: staged {len(fixture_manifest['cases'])} independently decoded guest fixtures", flush=True)
    if args.xz_qt_helper:
        prepare.write(init, "etc/rufus-xz-qt-fixture", "xz-qt\n")
    if args.bzip2_qt_helper:
        prepare.write(init, "etc/rufus-bzip2-qt-fixture", "bzip2-qt\n")
    if args.lzma_qt_helper:
        prepare.write(init, "etc/rufus-lzma-qt-fixture", "lzma-qt\n")
    if args.zip_qt_helper:
        prepare.write(init, "etc/rufus-zip-qt-fixture", "zip-qt\n")
    if args.zstd_qt_helper:
        prepare.write(init, "etc/rufus-zstd-qt-fixture", "zstd-qt\n")
    if args.compressed_qt_helper or args.bzip2_qt_helper or args.lzma_qt_helper or args.lzw_qt_helper or args.zip_qt_helper or args.zstd_qt_helper or args.vtsi_qt_helper or args.vhd_qt_helper:
        prepare.write(init, "etc/rufus-compressed-qt-fixture", "compressed-qt\n")
    if args.ntfs_mounted_helper or args.ntfs_mount_driver or args.save_export or args.media_appearance or args.ntfs_options or args.iso_resources or args.legacy_iso or args.geometry_batch:
        for package in (args.ntfs_3g_package, args.ntfs_library_package):
            run(['bsdtar', '-xpf', str(package), '--no-same-owner', '--no-xattrs',
                 '--exclude=.PKGINFO', '--exclude=.BUILDINFO', '--exclude=.MTREE', '--exclude=.INSTALL', '-C', str(init)])
        # Guest-only packages; the host does not need an NTFS-3G installation.
        shutil.copy2('/usr/lib/libfuse.so.2', init / 'usr/lib/libfuse.so.2')
        if args.ntfs_mounted_helper:
            prepare.write(init, 'etc/rufus-ntfs-mounted-fixture', 'NTFS-3G mounted helper\n')
    if args.fixed_usb_helper:
        prepare.write(init, "etc/rufus-fixed-usb-fixture", "fixed-usb\n")
        for tool in ('dmsetup', 'mkdir', 'cp'):
            add_executable(init, Path('/usr/bin') / tool)
    extra_modules = ["ntfs", "nls_utf8", "exfat", "fuse"] if args.ffu_optimized else ["ntfs", "nls_utf8", "exfat"] if args.media_appearance or args.old_bios else ["fuse"] if args.save_export else []
    if args.runtime_validation or args.iso_options or args.ntfs_options or args.iso_resources or args.legacy_iso:
        extra_modules.extend(["ntfs", "nls_utf8"])
    if args.native_formats or args.geometry_batch:
        extra_modules.extend(["udf", "ext4", "nls_utf8"])
    if args.geometry_batch:
        extra_modules.extend(["ntfs", "exfat", "fuse", "uas"])
    if args.windows_togo:
        extra_modules.extend(['ntfs', 'ntfs3', 'nls_utf8'])
    if args.fixed_usb_helper:
        extra_modules.append('dm_mod')
    if args.ntfs_mounted_helper or args.ntfs_mount_driver or args.save_export or args.media_appearance or args.ntfs_options or args.iso_resources or args.legacy_iso or args.geometry_batch:
        extra_modules.append('fuse')
    if args.ntfs_mount_driver == 'ntfs':
        extra_modules.extend(['ntfs', 'nls_utf8'])
    if args.device_options:
        extra_modules.extend(["virtio_pci", "virtio_scsi"])
    if args.device_source:
        extra_modules.extend(["loop", "nbd", "sr_mod", "btrfs", "dm_mod", "overlay"])
    for module_name in extra_modules:
        kernel = Path('/usr/lib/modules') / args.kernel
        modules = subprocess.check_output(['modprobe', '--show-depends', '-S', args.kernel, module_name], text=True)
        for line in modules.splitlines():
            if line.startswith('insmod '):
                module = Path(line.split()[1]).resolve()
                destination = init / 'usr/lib/modules' / args.kernel / module.relative_to(kernel)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(module, destination)
        run(['depmod', '-b', str(init), args.kernel])
    if args.mounted_raw:
        prepare.write(init, "etc/rufus-mounted-raw-fixture", "mounted-raw\n")
    if args.mounted_iso_helper:
        prepare.write(init, "etc/rufus-mounted-iso-helper-fixture", "mounted-iso-helper\n")
    if args.exfat_helper:
        prepare.write(init, "etc/rufus-exfat-helper-fixture", "exfat-helper\n")
    if args.ntfs_mount_driver:
        prepare.write(init, "etc/rufus-ntfs-mount-fixture", args.ntfs_mount_driver + "\n")
    if args.grub4dos_ntfs and args.ntfs_mount_driver == "lowntfs-3g":
        provider = init / "usr/bin/lowntfs-3g"
        provider.rename(provider.with_name("lowntfs-3g.real"))
        prepare.write(init, "usr/bin/lowntfs-3g", '''#!/bin/bash
set -u
[[ $(< /sys/class/dmi/id/product_name) == RufusMBRFixture ]] || exit 77
[[ $# == 4 && $3 == -o && $4 == *no_detach* ]] || exit 64
/usr/bin/lowntfs-3g.real "$@" &
native=$!
echo "$$ $PPID $native" > /run/grub4dos-fuse-processes
wait "$native"
result=$?
echo "$result" > /run/grub4dos-fuse-exit-held
for attempt in {1..1000}; do
    [[ -f /run/grub4dos-fuse-exit-release ]] && exit "$result"
    sleep .01
done
exit 78
''', 0o755)

    if args.windows_bios:
        (init / "tmp").mkdir(mode=0o1777, exist_ok=True)
        (init / "root").mkdir(exist_ok=True)
        shutil.copy2(args.iso, init / "source.iso")
        choice = args.windows_bios + (" dual" if args.windows_dual else " bios")
        choice += " fat32" if args.windows_fat32 else " ntfs"
        choice += " repair" if args.windows_efi_repair else ""
        choice += " winpe" if args.windows_winpe else ""
        choice += " syslinux-data" if args.windows_winpe_syslinux else ""
        choice += " grub-data-" + args.windows_winpe_grub_data if args.windows_winpe_grub_data else ""
        if args.windows_winpe_grub_data and not args.windows_winpe_syslinux:
            choice += " grub4dos-sbr" if args.windows_winpe_grub_data == "grub4dos" else " grub2-sbr"
        prepare.write(init, "etc/rufus-windows-bios-fixture", choice + "\n")
        if args.windows_winpe_failure:
            prepare.write(init, "etc/rufus-winpe-failure-fixture", "root write failure\n")
            prepare.write(init, "etc/rufus-extract-fixture", "legacy root write fault\n")
            prepare.write(init, "home/rufus/extract-fault", "winpe-write")
    if args.grub2_image:
        (init / "tmp").mkdir(mode=0o1777, exist_ok=True)
        (init / "root").mkdir(exist_ok=True)
        shutil.copy2(args.iso, init / "source.iso")
        shutil.copy2(args.grub_core, init / "selected-core.img")
        prepare.write(init, "etc/rufus-grub2-image-fixture", "uefi\n" if args.grub_uefi else "image\n")
    if args.grub4dos_image:
        shutil.copy2(args.iso, init / "source.iso")
        prepare.write(init, "etc/rufus-grub4dos-image-fixture", "image\n")
        if args.grub4dos_image_uefi:
            prepare.write(init, "etc/rufus-grub4dos-image-uefi-fixture", "BIOS/UEFI\n")
    if args.grub4dos_helper:
        shutil.copy2(prepare.HERE.parent / "fixtures/download/grub4dos-0.4.6a/grldr", init / "selected-loader")
        (init / "tmp").mkdir(mode=0o1777, exist_ok=True)
        prepare.write(init, "etc/rufus-grub4dos-fixture", "grub4dos\n")
        if args.grub4dos_ntfs:
            prepare.write(init, "etc/rufus-grub4dos-ntfs-fixture", "ntfs\n")
    if args.grub2_helper or args.grub2_ntfs_helper or args.grub2_ext_helper:
        prepare.write(init, "etc/rufus-grub2-fixture", "grub2\n")
    if args.grub2_ext_helper:
        prepare.write(init, "etc/rufus-grub2-ext-helper-fixture", f"{args.ext_bits}\n")
    if args.qt_gui:
        marker = "etc/rufus-winpe-gui-fixture" if args.windows_winpe_gui else "etc/rufus-grub2-ext-gui-fixture"
        if args.grub4dos_gui:
            marker = "etc/rufus-grub4dos-gui-fixture"
            cache = init / "grub4dos-files/grub4dos-0.4.6a"
            cache.mkdir(parents=True)
            shutil.copy2(init / "selected-loader", cache / "grldr")
        if args.grub4dos_image_gui:
            add_executable(init, Path("/usr/bin/tee"))
            marker = "etc/rufus-grub4dos-image-gui-fixture"
        prepare.write(init, marker, "GUI\n")
        plugins = init / "usr/lib/qt6/plugins/platforms"
        plugins.mkdir(parents=True)
        shutil.copy2(init / "usr/bin/libqoffscreen.so", plugins / "libqoffscreen.so")
    if args.fat_helper:
        prepare.write(init, "etc/rufus-fat-helper-fixture", "fat-helper\n")
    if args.freedos_helper:
        prepare.write(init, "etc/rufus-freedos-fixture", "freedos\n")
        prepare.write(init, "etc/rufus-extract-fixture", "freedos copy faults\n")
        (init / "home/rufus").mkdir(parents=True)
        if args.freedos_win7:
            prepare.write(init, "etc/rufus-freedos-win7-fixture", "win7\n")
    if args.syslinux_ntfs:
        prepare.write(init, "etc/rufus-syslinux-ntfs-fixture", args.syslinux_ntfs + "\n")
        if args.syslinux_image:
            prepare.write(init, "etc/rufus-syslinux-image-fixture", "uefi\n" if args.syslinux_uefi else "image\n")
            if args.syslinux_uefi:
                shutil.copy2(base / "bios.iso", init / "bios.iso")
            shutil.copy2(args.iso, init / "source.iso")
            for name in ("unknown", "incompatible", "conflict", "efi"):
                shutil.copy2(base / (name + ".iso"), init / (name + ".iso"))
        destination = init / args.installer_path.lstrip("/")
        destination.rename(destination.with_suffix(".real"))
        prepare.write(init, str(destination.relative_to(init)), '''#!/bin/bash
set -eu
/usr/bin/test_mbr_handoff_guest --claim
if [[ $(< /home/rufus/syslinux-fault) == pause ]]; then
    echo paused > /run/syslinux-paused
    sleep 60
fi
exec "${0}.real" "$@"
''', 0o755)
    if args.syslinux_major:
        prepare.write(init, "etc/rufus-syslinux-fixture", f"{args.syslinux_major}\n")
        (init / "home/rufus").mkdir(parents=True)
        (init / "usr/bin/sh").symlink_to("bash")
        (init / "usr/bin/mcopy").rename(init / "usr/bin/mcopy.real")
        prepare.write(init, "usr/bin/mcopy", '''#!/bin/bash
set -eu
/usr/bin/test_mbr_handoff_guest --claim
if [[ $(< /home/rufus/syslinux-fault) == pause ]]; then
    echo paused > /run/syslinux-paused
    sleep 60
fi
exec -a mcopy /usr/bin/mcopy.real "$@"
''', 0o755)
    if args.fat_bits == 16:
        prepare.write(init, "etc/rufus-fat16-fixture", "fat16\n")
    prepare.write(init, "etc/rufus-mbr-fixture", "RUFUS-MBR-TEST\n")
    prepare.write(init, "init", (prepare.HERE / "guest-mbr").read_text(), 0o755)
    names = run(["find", ".", "-print0"], cwd=init, capture_output=True).stdout
    with (base / "initramfs.cpio").open("wb") as output:
        subprocess.run(["cpio", "--null", "-o", "-H", "newc", "--owner=0:0"], cwd=init, input=names,
            stdout=output, check=True, timeout=INITRAMFS_PACK_TIMEOUT_SECONDS)


def run_guest(args, base, disk, sector):
    serial = base / f"serial-{sector}.log"
    memory_mib = 256
    qt_gui = args.qt_gui
    # Qt plus debug/sanitizer binaries need room for both the cpio archive
    # and its unpacked root before the kernel releases the archive pages.
    if args.alpine or qt_gui or args.compressed_qt_helper or args.bzip2_qt_helper or args.lzma_raw_helper or args.lzma_qt_helper or args.lzw_raw_helper or args.lzw_qt_helper or args.zip_raw_helper or args.zip_qt_helper or args.zstd_raw_helper or args.zstd_qt_helper or args.vtsi_raw_helper or args.vtsi_qt_helper or args.vhd_raw_helper:
        memory_mib = 2048
    elif args.windows_bios or args.syslinux_major or args.freedos_helper or args.helper or args.exfat_helper or args.fat_helper or args.ntfs_helper or args.grub2_ext_helper:
        memory_mib = 1024
    elif args.handoff or args.install:
        memory_mib = 512
    if args.windows_togo or args.vhdx_qt_helper or args.ffu_qt_helper or args.save_qt_helper or args.zip_supplement or args.native_formats or args.runtime_validation or args.legacy_boot or args.iso_options or args.ntfs_options or args.iso_resources:
        memory_mib = 2048
    if args.device_options: memory_mib = 2048
    if args.iso_resources: memory_mib = 4096 # Instrumented initramfs plus the independent NTFS readback copy.
    if args.legacy_iso: memory_mib = 4096 # Instrumented Qt/initramfs and native NTFS fixture providers.
    if args.geometry_batch: memory_mib = 4096 # Instrumented Qt and all seven native providers in one guest.
    command = ["qemu-system-x86_64", "-accel", args.accel,
               "-cpu", "host" if args.accel == "kvm" else "max", "-m", str(memory_mib),
               "-display", "none", "-monitor", "none", "-serial", "file:" + str(serial),
               "-no-reboot", "-nic", "none", "-kernel", str(base / "vmlinuz"),
               "-initrd", str(base / "initramfs.cpio"),
               "-append", "console=ttyS0 panic=-1 rufus.mbr_fixture=1",
               "-smbios", "type=1,manufacturer=RufusTest,product=RufusMBRFixture",
               "-device", "qemu-xhci,id=xhci",
               "-drive", "if=none,id=stick,format=raw,file=" + str(disk),
               "-device", f"usb-storage,drive=stick,removable={'off' if args.fixed_usb_helper else 'on'},serial=RUFUS-MBR-TEST,"
               f"logical_block_size={sector},physical_block_size={sector}"]
    if args.geometry_batch:
        from geometry_batch import append_devices
        append_devices(command, base)
    if args.legacy_iso:
        for name in ('legacy-windows.raw', 'legacy-reactos.raw'):
            snapshot = base / name
            with snapshot.open('xb') as output:
                output.truncate(EXT_DISK_BYTES)
            command.extend(['-drive', 'if=virtio,format=raw,file=' + str(snapshot)])
    if args.device_options and not args.device_source:
        command.extend(['-device', 'virtio-scsi-pci,id=classes'])
        for index, vendor, product, removable in [(0, 'Msft', 'Virtual Disk', 'off'),
                (1, 'VMware', 'VMware Virtual S', 'off'), (2, 'ATA', 'External SATA', 'on')]:
            target = base / ('device-class-' + str(index) + '.raw')
            with target.open('xb') as output: output.truncate(64 * 1024 * 1024)
            command.extend(['-drive', 'if=none,id=class' + str(index) + ',format=raw,file=' + str(target),
                '-device', 'scsi-hd,bus=classes.0,drive=class' + str(index) + ',scsi-id=' + str(index)
                    + ',vendor=' + vendor + ',product=' + product + ',removable=' + removable])
    if args.device_source:
        command.extend(["-nic", "none"])
        disc = base / 'optical.raw'
        disc.write_bytes(bytes(range(256)) * 8192)
        command.extend(['-device', 'virtio-scsi-pci,id=optical', '-drive',
            'if=none,id=disc,format=raw,media=cdrom,readonly=on,file=' + str(disc),
            '-device', 'scsi-cd,bus=optical.0,drive=disc'])
    if args.save_qt_helper:
        export = base / "capture-export.raw"
        with export.open("wb") as file:
            file.truncate(320 * 1024 * 1024)
        run(["mkfs.ext4", "-q", "-F", str(export)])
        command.extend(["-drive", "if=virtio,format=raw,file=" + str(export)])
    if args.ffu_geometry:
        from test_image_save import gpt_image
        capture_source = base / "ffu-source.raw"
        if args.ffu_layouts:
            from test_ffu_layouts import layout_image
            capture_source.write_bytes(layout_image(entries=8192, entry_bytes=256, partitions=1024))
        else:
            capture_source.write_bytes(gpt_image(size=8 * MIB + 1536, name='EFI \u03a9'))
        command.extend(["-drive", "if=virtio,format=raw,readonly=on,file=" + str(capture_source)])
        (base / "ffu-source.json").write_text(json.dumps({'bytes': capture_source.stat().st_size,
            'sha256': digest(capture_source), 'read_only': True}, indent=2) + '\n')
    if args.old_bios:
        scratch = base / "old-bios-boot.raw"
        with scratch.open("wb") as stream: stream.truncate(320 * MIB)
        command.extend(["-drive", "if=virtio,format=raw,file=" + str(scratch)])
    (base / f"qemu-command-{sector}.json").write_text(json.dumps(command, indent=2) + "\n")
    timeout = NTFS_HELPER_TIMEOUT_SECONDS if args.ntfs_mounted_helper or args.syslinux_image else 180
    if args.native_formats or args.runtime_validation or args.legacy_boot or args.iso_options or args.ntfs_options or args.iso_resources or args.legacy_iso:
        timeout = 600
    if args.geometry_batch: timeout = 1800
    if args.windows_togo:
        timeout = 1200
    if args.bzip2_qt_helper:
        timeout = BZIP2_QT_HELPER_TIMEOUT_SECONDS
    if args.lzma_raw_helper:
        timeout = LZMA_HELPER_TIMEOUT_SECONDS
    if args.lzma_qt_helper:
        timeout = LZMA_QT_HELPER_TIMEOUT_SECONDS
    if args.lzw_raw_helper:
        timeout = LZW_HELPER_TIMEOUT_SECONDS
    if args.zip_raw_helper:
        timeout = ZIP_HELPER_TIMEOUT_SECONDS
    if args.zstd_raw_helper:
        timeout = ZSTD_HELPER_TIMEOUT_SECONDS
    if args.vtsi_raw_helper:
        timeout = VTSI_HELPER_TIMEOUT_SECONDS
    if args.vhd_raw_helper:
        timeout = 1800 if args.vhd_qt_helper else VTSI_HELPER_TIMEOUT_SECONDS
    if args.vhdx_qt_helper or args.ffu_qt_helper or args.save_qt_helper:
        timeout = 900
    if args.ffu_optimized:
        timeout = 1800
    if args.vtsi_qt_helper:
        timeout = 1800
    if args.zip_qt_helper:
        timeout = ZIP_QT_HELPER_TIMEOUT_SECONDS
    if args.zstd_qt_helper:
        timeout = ZSTD_QT_HELPER_TIMEOUT_SECONDS
    if args.lzw_qt_helper:
        timeout = LZW_QT_HELPER_TIMEOUT_SECONDS
    if args.compressed_raw_helper:
        timeout = BZIP2_HELPER_TIMEOUT_SECONDS if args.bzip2_raw_helper else XZ_QT_HELPER_TIMEOUT_SECONDS if args.xz_qt_helper else XZ_HELPER_TIMEOUT_SECONDS if args.xz_raw_helper else COMPRESSED_HELPER_TIMEOUT_SECONDS
    if args.grub2_helper or args.grub2_ntfs_helper or args.grub2_ext_helper:
        timeout = 600 if args.grub2_ext_helper else GRUB2_HELPER_TIMEOUT_SECONDS
    if args.device_source: timeout = 900
    with (base / f"qemu-{sector}.log").open("wb") as log:
        subprocess.run(command, check=True, timeout=timeout, stdout=log, stderr=subprocess.STDOUT)
    text = serial.read_text(errors="replace")
    for diagnostic in ("AddressSanitizer", "LeakSanitizer", "runtime error:"):
        assert diagnostic not in text, (diagnostic, serial)
    if args.geometry_batch:
        jobs = re.findall(r'RUFUS_GEOMETRY_QT_DONE FS=(\d+) LOGICAL=(\d+) PHYSICAL=(\d+) AUTH=1 JOINED=1 READY=100', text)
        readback = re.findall(r'RUFUS_GEOMETRY_READBACK JOB=(\d+) FS=(\d+) LOGICAL=(\d+) PHYSICAL=(\d+) LAYOUT=(MBR|GPT|SFD) BYTES=1 CLAIM=1', text)
        expected = []
        for geometry in (2, 1, 0):
            logical, physical = (4096, 4096) if geometry == 2 else (512, 4096) if geometry == 1 else (512, 512)
            for index, filesystem in enumerate((0, 2, 4, 3, 6, 7, 1)):
                expected.append((str(filesystem), str(logical), str(physical), ('MBR', 'GPT', 'SFD')[(index + geometry) % 3]))
        assert jobs == [item[:3] for item in expected], (jobs, expected)
        assert readback == [(str(index), *item) for index, item in enumerate(expected)], (readback, expected)
        assert re.findall(r'RUFUS_GEOMETRY_STALE LOGICAL=(\d+) PHYSICAL=(\d+) REFUSALS=2 BEFORE_AUTH=1', text) == [
            ('4096', '4096'), ('512', '4096'), ('512', '512')]
        assert re.findall(r'RUFUS_GEOMETRY_NTFS_PROVIDER LOGICAL=(\d+) PHYSICAL=(\d+) DRIVER=(ntfs-3g|ntfs) PREWRITE=1', text) == [
            ('4096', '4096', 'ntfs-3g'), ('512', '4096', 'ntfs'), ('512', '512', 'ntfs')]
        assert 'RUFUS_GEOMETRY_DONE JOBS=21 GEOMETRIES=3 FORMATTERS=7 AUTH=1 JOINED=1 READBACK=1' in text
        assert 'RUFUS_MBR_EXIT 0' in text
        return {'geometry_batch': {'jobs': 21, 'geometries': ['512/512', '512/4096', '4096/4096'],
            'formatters': ['fat16', 'fat32', 'ntfs', 'exfat', 'udf', 'ext2', 'ext3'],
            'layouts': ['MBR', 'GPT', 'SFD'], 'ordinary_qt': True, 'independent_kernel_readback': True,
            'joined_ready': True, 'fat32_full_format': True, 'stale_geometry_refusals': 6,
            'overlay_payload_formats': ['fat16', 'fat32', 'ntfs', 'exfat', 'udf'],
            'ext_clean_root_and_superblock': True,
            'ntfs_providers_before_partitioning': {'4096/4096': 'ntfs-3g', '512/4096': 'ntfs', '512/512': 'ntfs'}}}
    if args.device_source:
        assert 'RUFUS_DEVICE_SOURCE_MAPPED_RESTORE BYTES=1 AUTH=1 JOINED=1' in text
        assert 'RUFUS_DEVICE_SOURCE_DONE LOOP=1 NBD=1 OPTICAL=1 RESET=1 ESP=1 BACKING=1' in text
        assert 'RUFUS_MBR_EXIT 0' in text
        return {'device_source': {'loop': True, 'nbd': True, 'optical': True, 'reset': True, 'esp': True, 'backing': True}}
    if args.device_options:
        jobs = re.findall(r'RUFUS_DEVICE_OPTIONS_QT_DONE CLASS=(\d+) AUTH=1 JOINED=1 READY=100', text)
        readback = re.findall(r'RUFUS_DEVICE_OPTIONS_READBACK CLASS=(\d+) FILESYSTEM=1 CLAIM=1 READ=1', text)
        assert jobs == readback == ['2', '4', '8'], (jobs, readback)
        assert 'RUFUS_DEVICE_OPTIONS_HOME_PROTECTED=1' in text
        assert 'RUFUS_DEVICE_OPTIONS_DONE JOBS=3' in text and 'RUFUS_MBR_EXIT 0' in text
        return {'device_options': {'jobs': 3, 'ordinary_qt': True, 'independent_readback': True, 'joined_ready': True}}
    if args.iso_resources:
        jobs = re.findall(r"RUFUS_ISO_RESOURCES_QT_DONE FS=(\d+) AUTH=1 JOINED=1 READY=100",text)
        readback = re.findall(r"RUFUS_ISO_RESOURCES_READBACK JOB=(\d+) BYTES=1 LINKS=1 CLAIM=1",text)
        assert jobs == ['1','2','1','2'] and readback == ['0','1','2','3'], (jobs,readback)
        assert 'RUFUS_ISO_RESOURCES_DONE JOBS=4' in text and 'RUFUS_MBR_EXIT 0' in text
        return {'iso_resources': {'jobs':4,'ordinary_qt':True,'readback':True,'windows_reparse':True,'joined_ready':True}}
    if args.legacy_iso:
        jobs = re.findall(r'RUFUS_LEGACY_ISO_QT_DONE FS=(\d+) AUTH=1 JOINED=1 READY=100', text)
        readback = re.findall(r'RUFUS_LEGACY_ISO_READBACK JOB=(\d+) FILES=\d+ PBR=1 ATTRS=1 CLAIM=1', text)
        assert jobs == ['0','0','1','2','1'] and readback == ['0','1','2','4','3'], (jobs, readback)
        assert 'RUFUS_LEGACY_ISO_DONE JOBS=5' in text and 'RUFUS_LEGACY_ISO_NTFS_DUAL LAYOUT=1 FLAGS=1 PAYLOAD=1 CLEAN=1' in text and 'RUFUS_MBR_EXIT 0' in text
        return {'legacy_iso': {'jobs':5,'ordinary_qt':True,'readback':True,'fat_attributes':True,'joined_ready':True}}
    if args.ntfs_options:
        jobs = re.findall(r"RUFUS_NTFS_OPTIONS_QT_DONE JOB=(\d+) AUTH=1 JOINED=1 READY=100", text)
        readback = re.findall(r"RUFUS_NTFS_OPTIONS_READBACK JOB=(\d+) FLAGS=1 BYTES=1 CLAIM=1", text)
        assert jobs == readback == ['0', '1', '2'], (jobs, readback)
        assert 'RUFUS_NTFS_OPTIONS_DONE JOBS=3 PREWRITE_PROVIDER=1' in text and 'RUFUS_MBR_EXIT 0' in text
        return {'ntfs_options': {'jobs': 3, 'ordinary_qt': True, 'independent_readback': True, 'joined_ready': True}}
    if args.iso_options:
        jobs = re.findall(r"RUFUS_ISO_OPTIONS_QT_DONE FS=(\d+) AUTH=1 JOINED=1 READY=100", text)
        readback = re.findall(r"RUFUS_ISO_OPTIONS_READBACK FS=(\d+) TIMES=1 BYTES=1 CLAIM=1", text)
        assert jobs == readback == ['0', '1', '2'], (jobs, readback)
        assert 'RUFUS_ISO_OPTIONS_DONE JOBS=3' in text and 'RUFUS_MBR_EXIT 0' in text
        return {'iso_options': {'filesystems': jobs, 'ordinary_qt': True, 'independent_readback': True,
                                'joined_ready': True, 'dos_64k_override': True, 'iso_udf_timestamps': True}}
    if args.legacy_boot:
        jobs = re.findall(r"RUFUS_LEGACY_QT_DONE DOS=(\d+) FS=(\d+) AUTH=1 JOINED=1 READY=100",text)
        readback = re.findall(r"RUFUS_LEGACY_READBACK DOS=(\d+) FS=(\d+) FILES=\d+ ATTRS=1 CLAIM=1",text)
        assert jobs == readback == [('1','0'),('1','1'),('0','0'),('0','1')],(jobs,readback)
        assert 'RUFUS_LEGACY_DONE JOBS=4 PREWRITE_REFUSAL=1 AUTH=1 JOINED=1 READBACK=1' in text and 'RUFUS_MBR_EXIT 0' in text
        return {'legacy_boot':{'jobs':jobs,'ordinary_qt':True,'independent_readback':True,'joined_ready':True,'changed_resource_prewrite_refusal':True}}
    if args.runtime_validation:
        jobs = re.findall(r"RUFUS_RUNTIME_QT_DONE FS=(\d+) AUTH=1 JOINED=1 READY=100", text)
        readback = re.findall(r"RUFUS_RUNTIME_READBACK FS=(\d+) HASHES=1 ORIGINAL=1 RESOURCE=1 CLAIM=1", text)
        assert jobs == readback == ['0', '1', '2'], (jobs, readback)
        assert 'RUFUS_RUNTIME_DONE JOBS=3 PREWRITE_CONFLICT=1 AUTH=1 JOINED=1 READBACK=1' in text and 'RUFUS_MBR_EXIT 0' in text, serial
        return {'runtime_validation': {'filesystems': jobs, 'ordinary_qt': True, 'independent_readback': True,
                                      'joined_ready': True, 'prewrite_backup_conflict': True}}
    if args.native_formats:
        jobs = re.findall(r"RUFUS_NATIVE_QT_DONE FS=(\d+) SFD=(\d+) MBR=(\d+) QUICK=(\d+) AUTH=1 JOINED=1 READY=100", text)
        readback = re.findall(r"RUFUS_NATIVE_READBACK FS=(\d+) SFD=(\d+) MBR=(\d+) QUICK=(\d+)", text)
        if args.udf_indexing_batch:
            assert jobs == readback == [('3', '0', '0', '1'), ('3', '1', '0', '1')], (jobs, readback)
            choices = re.findall(r"RUFUS_UDF_INDEXING_QT_DONE INDEXING=(\d+) DIAGNOSTIC=(\d+) AUTH=1 JOINED=1 READY=100", text)
            payloads = re.findall(r"RUFUS_UDF_INDEXING_READBACK INDEXING=(\d+) LABEL=1 ICON=1 ZIP=1 CLAIM=1", text)
            assert choices == [('0', '1'), ('1', '0')] and payloads == ['0', '1'], (choices, payloads)
            assert 'RUFUS_UDF_INDEXING_DONE JOBS=2 AUTH=1 JOINED=1 READBACK=1' in text and 'RUFUS_MBR_EXIT 0' in text
            result = {'udf_indexing': {'jobs': jobs, 'choices': choices, 'ordinary_qt': True,
                'saved_choice': True, 'optional_diagnostic': True, 'independent_readback': True, 'joined_ready': True}}
            (base / 'udf-indexing-results.json').write_text(json.dumps(result, indent=2) + '\n')
            return result
        assert jobs == readback == [('3', '0', '0', '1'), ('3', '1', '0', '0'), ('6', '1', '0', '1'), ('7', '0', '1', '0')], (jobs, readback)
        assert 'RUFUS_NATIVE_FORMATS_DONE JOBS=4 AUTH=1 JOINED=1 READBACK=1' in text and 'RUFUS_MBR_EXIT 0' in text
        result = {'native_formats': {'jobs': jobs, 'ordinary_qt': True, 'independent_readback': True, 'joined_ready': True}}
        (base / 'native-format-results.json').write_text(json.dumps(result, indent=2) + '\n')
        return result
    if args.windows_togo:
        assert text.count('RUFUS_TOGO_QT_DONE ') == 4, serial
        assert text.count('RUFUS_TOGO_READBACK ') == 4, serial
        assert 'RUFUS_TOGO_DONE FORMATS=4 AUTH=1 JOINED=1' in text and 'RUFUS_MBR_EXIT 0' in text, serial
        return {'windows_togo': {'formats': 4, 'ordinary_qt': True, 'edition': 2, 'joined_ready': True}}
    if args.zip_supplement:
        assert "RUFUS_ARCHIVE_QT_DONE AUTH=1 JOINED=1 READY=100 ARCHIVE=1" in text
        assert "RUFUS_ARCHIVE_DONE PREWRITE_CRC=1 SOURCE_PROTECTED=1 AUTH=1 JOINED=1 BOOTABLE_ISO=1" in text
        assert "RUFUS_MBR_EXIT 0" in text
        return {"archive": {"prewrite_crc": True, "source_protected": True, "ordinary_qt": True, "joined_ready": True}}
    if args.grub4dos_helper:
        assert "RUFUS_GRUB4DOS_DONE" in text and "RUFUS_MBR_EXIT 0" in text, serial
        report = values(re.search(r"RUFUS_GRUB4DOS_DONE ([^\r\n]+)", text).group(1))
        assert report["ERROR"] == 0 and report["COPIED"] == 322965, report
        if args.grub4dos_ntfs:
            driver_log = "NTFS file copying uses the kernel ntfs driver" if args.ntfs_mount_driver == "ntfs" else "NTFS file copying uses ntfs-3g"
            assert driver_log in text, serial
            if args.ntfs_mount_driver == "lowntfs-3g":
                assert "RUFUS_GRUB4DOS_FUSE_JOINED" in text, serial
        if args.grub4dos_gui:
            assert "RUFUS_GRUB4DOS_GUI_DONE" in text, serial
            assert "VOLUMEID" in report, report
        return {"success": report}
    if args.grub4dos_image:
        assert "RUFUS_GRUB4DOS_IMAGE_DONE" in text and "RUFUS_MBR_EXIT 0" in text, serial
        assert "RUFUS_IMAGE_GRUB4DOS_SBR_DONE" in text, serial
        if args.grub4dos_image_uefi:
            assert "RUFUS_GRUB4DOS_IMAGE_DUAL_REQUEST_DONE" in text, serial
        if args.grub4dos_ntfs:
            driver_log = "NTFS file copying uses the kernel ntfs driver" if args.ntfs_mount_driver == "ntfs" else "NTFS file copying uses ntfs-3g"
            assert driver_log in text, serial
            if args.ntfs_mount_driver == "lowntfs-3g":
                assert "RUFUS_GRUB4DOS_FUSE_JOINED" in text, serial
        if args.grub4dos_image_gui:
            assert "RUFUS_GRUB4DOS_IMAGE_GUI_DONE" in text, serial
        return {"image-success": values(re.search(r"RUFUS_GRUB4DOS_IMAGE_DONE ([^\r\n]+)", text).group(1))}
    if args.windows_bios:
        assert "RUFUS_WINDOWS_IMAGE_DONE" in text and "RUFUS_MBR_EXIT 0" in text, serial
        if args.windows_winpe_gui:
            assert "RUFUS_WINPE_GUI_DONE" in text, serial
        outcome = "image-failure" if args.windows_winpe_failure else "image-success"
        return {outcome: values(re.search(r"RUFUS_WINDOWS_IMAGE_DONE ([^\r\n]+)", text).group(1))}
    if args.grub2_image:
        assert "RUFUS_GRUB2_IMAGE_DONE" in text and "RUFUS_MBR_EXIT 0" in text, serial
        return {"image-success": values(re.search(r"RUFUS_GRUB2_IMAGE_DONE ([^\r\n]+)", text).group(1))}
    if args.grub2_ext_gui:
        assert "RUFUS_GRUB2_EXT_GUI_DONE" in text and "RUFUS_MBR_EXIT 0" in text, serial
        assert text.count("RUFUS_FORMAT_VOLUME ") == 1, serial
        return {"gui-success": {"ERROR": 0}}
    if args.grub2_ext_helper:
        assert f"RUFUS_GRUB2_EXT{args.ext_bits}_DONE" in text and "RUFUS_MBR_EXIT 0" in text, serial
        reports = {name: values(report) for name, report in
                   re.findall(r"RUFUS_FORMAT_HELPER_CASE (\S+) ([^\r\n]+)", text)}
        expected = {"success"}
        if args.ext_bits == 3:
            expected |= {"wrong-sequence", "missing-formatter", "bad-label", "formatter-failure",
                         "cancel-formatter", "mbr-changed", "busy-reacquire", "sync-fail", "cancel-sync",
                         "core-write-fail", "core-corrupt", "core-cancel"}
        assert set(reports) == expected and reports["success"]["ERROR"] == 0, (reports, serial)
        assert text.count("RUFUS_FORMAT_VOLUME ") == 1, serial
        return reports
    if args.grub2_ntfs_helper:
        assert "RUFUS_GRUB2_NTFS_DONE" in text and "RUFUS_MBR_EXIT 0" in text, serial
        reports = {name: values(report) for name, report in
                   re.findall(r"RUFUS_FORMAT_HELPER_CASE (\S+) ([^\r\n]+)", text)}
        expected = {stage + '-' + fault for stage in ('core', 'pbr')
                    for fault in ('write-fail', 'sync-fail', 'corrupt', 'cancel')}
        expected.add('success')
        assert set(reports) == expected and reports['success']['ERROR'] == 0, (reports, serial)
        for stage in ('core', 'pbr'):
            assert f'RUFUS_GRUB2_CANCEL_SYNC {stage}' in text, serial
        assert text.count("RUFUS_FORMAT_VOLUME ") == 1, serial
        return reports
    if args.syslinux_major:
        assert "RUFUS_SYSLINUX_DONE" in text and "RUFUS_MBR_EXIT 0" in text, serial
        reports = {name: values(report) for name, report in re.findall(r"RUFUS_SYSLINUX_CASE (\S+) ([^\r\n]+)", text)}
        expected = {"wrong-sequence", "missing-installer", "pause", "failure", "no-op", "flush",
                    "read", "corrupt", "cleanup", "final-flush", "success"}
        if args.syslinux_ntfs:
            expected.update({"attributes", "mount-flush", "unmount", "missing-formatter"})
            if args.syslinux_ntfs == "ntfs": expected.add("mount")
        if args.syslinux_image:
            expected.update({"stale-source", "incompatible-source", "conflicting-config", "extra-partition",
                             "extract-failure", "extract-pause", "source-after-copy", "source-after-install", "unknown-success"})
        if args.syslinux_uefi:
            expected.update({"unneeded-partition", "bad-efi", "uefi-failure", "uefi-pause", "uefi-flush"})
        assert set(reports) == expected, reports
        return reports
    if args.freedos_helper:
        assert "RUFUS_FREEDOS_DONE" in text and "RUFUS_MBR_EXIT 0" in text, serial
        reports = {name: values(report) for name, report in re.findall(r"RUFUS_FREEDOS_CASE (\S+) ([^\r\n]+)", text)}
        assert set(reports) == {"wrong-sequence", "mount", "pause", "flush", "unmount", "success"}, reports
        return reports
    if args.sfd:
        assert "RUFUS_MBR_EXIT 0" in text, serial
        reports = {name: values(report) for name, report in re.findall(r"RUFUS_SFD_CASE (\S+) ([^\r\n]+)", text)}
        expected = {"table-still-present", "wrong-offset", "wrong-size", "invalid-cluster", "busy-child",
                    "cancel-before", "formatter-failure", "sync-fail", "busy-reacquire", "cancel-after",
                    "nested-mbr", "changed-bpb", "success"}
        if sector != 512:
            expected = {"unsupported-sector"}
        else:
            assert "RUFUS_SFD_DONE" in text, serial
        assert set(reports) == expected, reports
        return reports
    if args.exfat_helper or args.fat_helper or args.ntfs_helper:
        assert "RUFUS_FORMAT_HELPER_DONE" in text and "RUFUS_MBR_EXIT 0" in text, serial
        reports = {name: values(report) for name, report in re.findall(r"RUFUS_FORMAT_HELPER_CASE (\S+) ([^\r\n]+)", text)}
        expected = {"missing-formatter", "old-formatter", "bad-label", "metadata-space", "wrong-sequence",
                    "formatter-failure", "cancel-formatter", "sync-fail", "cancel-sync", "sync-eintr", "success",
                    "namespace-create", "namespace-private"}
        if args.fat_helper or args.ntfs_helper:
            expected -= {"old-formatter", "metadata-space"}
            expected.add("invalid-cluster")
        if args.mbr_fat_helper or args.mbr_ntfs_helper or args.mbr_exfat_helper or args.grub2_helper:
            expected.add("mbr-changed")
            assert "RUFUS_FORMAT_MBR_GPT_CLEARED" in text, serial
        if args.sfd_helper or args.sfd_ntfs_helper or args.sfd_exfat_helper:
            expected.add("sfd-table")
            assert "RUFUS_FORMAT_MBR_GPT_CLEARED" in text, serial
        if args.mbr_fat_helper or args.mbr_exfat_helper or args.sfd_helper or args.sfd_exfat_helper or args.grub2_helper:
            expected.add("busy-reacquire")
        if args.grub2_helper:
            expected |= {stage + '-' + fault for stage in ('core', 'pbr')
                         for fault in ('write-fail', 'sync-fail', 'corrupt', 'cancel')}
            for stage in ('core', 'pbr'):
                assert f'RUFUS_GRUB2_CANCEL_SYNC {stage}' in text, serial
        if args.sfd_exfat_helper:
            assert "RUFUS_SFD_EXFAT_PREFLIGHT_OK" in text, serial
        if args.sfd_ntfs_helper:
            assert "RUFUS_SFD_NTFS_PREFLIGHT_OK" in text, serial
        if args.standalone_uefi:
            expected |= {"uefi-sync-fail", "uefi-cancel-sync", "uefi-sync-eintr", "uefi-bootable-regression"}
        elif args.ntfs_helper:
            expected.add("bootable-regression")
        if args.ntfs_helper or args.standalone_uefi:
            assert "RUFUS_NTFS_BOOTABLE_PAYLOAD_OK" in text, serial
        assert set(reports) == expected, reports
        if args.ntfs_helper:
            assert text.count("RUFUS_INSTALL_CHILD_CLAIM_OK") == (7 if args.mbr_ntfs_helper or args.sfd_ntfs_helper else 6), serial
        assert "RUFUS_FORMAT_FINAL_SYNC EINTR=1 CANCEL=0" in text, serial
        assert "RUFUS_FORMAT_FINAL_SYNC EINTR=0 CANCEL=1" in text, serial
        if args.mounted_helper:
            mounted = {name: values(report) for name, report in re.findall(r"RUFUS_MOUNT_HELPER_CASE (\S+) ([^\r\n]+)", text)}
            expected_mounts = {"metadata-missing", "mount-mismatch", "home-target", "busy", "wrong-sequence",
                               "wrong-owner", "source-target", "cancel-after", "stale-cache", "service-gone",
                               "remounted", "success"}
            assert set(mounted) == expected_mounts and 'RUFUS_MOUNT_HELPER_DONE' in text, mounted
            (base / 'mounted-results.json').write_text(json.dumps(mounted, indent=2) + '\n')
        return reports
    if args.helper:
        for marker in ("RUFUS_HELPER_DONE", "RUFUS_MBR_EXIT 0"):
            assert marker in text, (marker, serial)
        reports = {name: values(report) for name, report in re.findall(r"RUFUS_HELPER_CASE (\S+) ([^\r\n]+)", text)}
        expected = {"alpine"} if args.alpine else {"missing-installer", "incompatible-source", "cancel-installer", "known-success", "unknown-success"}
        if args.vhdx_qt_helper or args.ffu_qt_helper or args.save_qt_helper:
            expected = {"unknown-success"}
        assert set(reports) == expected, reports
        if args.mounted_iso_helper:
            mounted = {name: values(report) for name, report in re.findall(r"RUFUS_MOUNT_HELPER_CASE (\S+) ([^\r\n]+)", text)}
            assert set(mounted) == {"iso-source-changed", "iso-cancel-after", "iso-success"}, mounted
            assert 'RUFUS_MOUNT_ISO_HELPER_DONE' in text, serial
            (base / 'mounted-results.json').write_text(json.dumps(mounted, indent=2) + '\n')
        if args.save_qt_helper:
            saved = {name: values(report) for name, report in re.findall(r"RUFUS_SAVE_QT_DONE (\S+) ([^\r\n]+)", text)}
            expected_saved = {"vhd", "mounted-vhdx", "iso", "ffu"} if args.save_read_only else {"iso", "ntfs-iso", "mounted-vhdx", "iso-cleanup", "busy", "helper-cancel"} if args.save_export else {"vhd", "vhdx", "ffu", "cancel", "deny"}
            assert set(saved) == expected_saved, saved
            (base / "capture-results.json").write_text(json.dumps(saved, indent=2) + "\n")
        if args.mounted_raw_helper:
            raw = {name: values(report) for name, report in re.findall(r"RUFUS_MOUNT_HELPER_CASE (\S+) ([^\r\n]+)", text)}
            expected_raw = {"raw-source-changed", "busy", "raw-cancel-after", "raw-success", "raw-unmounted"}
            if args.vhdx_qt_helper or args.ffu_qt_helper or args.save_qt_helper:
                expected_raw = set()
            if args.compressed_raw_helper:
                expected_raw |= {"gzip-busy", "gzip-home-target", "gzip-raw-cancel-after", "gzip-raw-success"}
                compressed = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_HELPER_CASE (\S+) ([^\r\n]+)", text)}
                expected_compressed = {"digest": errno.ESTALE, "size": errno.ESTALE, "sector": errno.ESTALE,
                                       "crc": errno.EBADMSG, "write": errno.EIO, "cancel": errno.ECANCELED,
                                       "sync": errno.EIO, "verify": errno.EBADMSG, "close": errno.EIO,
                                       "observed-success": 0, "bootable-restore": 0}
                base_compressed = dict(expected_compressed)
                if args.xz_raw_helper:
                    expected_raw |= {"xz-busy", "xz-home-target", "xz-raw-cancel-after", "xz-raw-success"}
                    expected_compressed.update({"xz-" + name: error for name, error in dict(expected_compressed).items()})
                    expected_compressed.update({"xz-crc32-crc": errno.EBADMSG, "xz-crc32-success": 0,
                                                "xz-sha256-crc": errno.EBADMSG, "xz-sha256-success": 0, "xz-none-success": 0})
                    assert "RUFUS_XZ_HELPER_DONE SHA256=" in text, serial
                if args.bzip2_raw_helper:
                    expected_raw |= {"bzip2-busy", "bzip2-home-target", "bzip2-raw-cancel-after", "bzip2-raw-success"}
                    expected_compressed.update({"bzip2-" + name: error for name, error in base_compressed.items()})
                    expected_compressed.update({"bzip2-" + name: errno.EBADMSG for name in
                                                ("combined-crc", "empty-crc-only", "empty-crc-first", "empty-crc-last", "empty-crc-middle")})
                    expected_compressed.update({"bzip2-concatenated-success": 0, "bzip2-tail-success": 0})
                    assert "RUFUS_BZIP2_HELPER_DONE SHA256=" in text, serial
                assert set(compressed) == set(expected_compressed), compressed
                assert all(compressed[name]["ERROR"] == error for name, error in expected_compressed.items()), compressed
                claims = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_CLAIM_CASE (\S+) ([^\r\n]+)", text)}
                expected_claims = {"write", "cancel", "sync", "verify", "close", "observed-success"}
                base_claims = set(expected_claims)
                if args.xz_raw_helper:
                    expected_claims |= {"xz-" + name for name in expected_claims}
                if args.bzip2_raw_helper:
                    expected_claims |= {"bzip2-" + name for name in base_claims}
                assert set(claims) == expected_claims, claims
                assert all(report["SYNCS"] == 1 and report["CLOSES"] == 1 and report["CLAIMS"] >= 5 for report in claims.values()), claims
                if args.compressed_qt_helper:
                    qt_frames = re.findall(r"RUFUS_COMPRESSED_QT_DONE ([^\r\n]+)", text)
                    qt = {values(report)["TYPE"]: values(report) for report in qt_frames}
                    expected_types = {3, 6} if args.xz_qt_helper else {3}
                    assert len(qt_frames) == len(expected_types) and set(qt) == expected_types, qt
                    assert all(report["WRITTEN"] == HANDOFF_DISK_BYTES and report["VERIFIED"] == HANDOFF_DISK_BYTES
                               for report in qt.values()), qt
                    (base / "compressed-qt-results.json").write_text(json.dumps(qt, indent=2) + "\n")
                assert "RUFUS_COMPRESSED_HELPER_DONE SHA256=" in text, serial
                (base / "compressed-helper-results.json").write_text(json.dumps({"cases": compressed, "claims": claims}, indent=2) + "\n")
            if args.lzma_raw_helper:
                expected_raw |= {"lzma-" + name for name in ("busy", "home-target", "raw-cancel-after", "raw-success")}
                compressed = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_HELPER_CASE (\S+) ([^\r\n]+)", text)}
                expected_compressed = {"digest": errno.ESTALE, "size": errno.ESTALE, "sector": errno.ESTALE,
                                       "size-header": errno.EBADMSG, "write": errno.EIO, "cancel": errno.ECANCELED,
                                       "sync": errno.EIO, "verify": errno.EBADMSG, "close": errno.EIO,
                                       "observed-success": 0, "source-read": errno.EIO, "source-eof": errno.EIO,
                                       "known-success": 0, "known-no-eos-success": 0, "second-stream-success": 0,
                                       "tail-success": 0, "properties": errno.EBADMSG, "dictionary": errno.ENOTSUP,
                                       "history": errno.EBADMSG, "declared-over-4gib": errno.EBADMSG,
                                       "known-overshoot": errno.EBADMSG, "truncated-eos": errno.EBADMSG,
                                       "maximum-dictionary-known-success": 0, "bootable-restore": 0}
                expected_compressed = {"lzma-" + name: error for name, error in expected_compressed.items()}
                assert len(expected_compressed) == 24 and set(compressed) == set(expected_compressed), compressed
                assert all(compressed[name]["ERROR"] == error for name, error in expected_compressed.items()), compressed
                claims = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_CLAIM_CASE (\S+) ([^\r\n]+)", text)}
                expected_claims = {"lzma-" + name for name in
                                   ("write", "cancel", "sync", "verify", "close", "observed-success", "source-read", "source-eof")}
                assert set(claims) == expected_claims, claims
                assert all(report["SYNCS"] == 1 and report["CLOSES"] == 1 and report["CLAIMS"] >= 5 for report in claims.values()), claims
                for name in ("lzma-source-read", "lzma-source-eof"):
                    assert claims[name]["SOURCE_READS"] >= 4 and compressed[name]["WRITTEN"] == 65536, (name, claims, compressed)
                assert "RUFUS_LZMA_HELPER_DONE SHA256=" in text and "RUFUS_COMPRESSED_QT_DONE" not in text, serial
                (base / "compressed-helper-results.json").write_text(json.dumps({"cases": compressed, "claims": claims}, indent=2) + "\n")
            if args.lzw_raw_helper:
                expected_raw |= {"lzw-" + name for name in ("busy", "home-target", "raw-cancel-after", "raw-success")}
                compressed = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_HELPER_CASE (\S+) ([^\r\n]+)", text)}
                expected_compressed = {"digest": errno.ESTALE, "size": errno.ESTALE, "sector": errno.ESTALE,
                                       "code": errno.EBADMSG, "write": errno.EIO, "cancel": errno.ECANCELED,
                                       "sync": errno.EIO, "verify": errno.EBADMSG, "close": errno.EIO,
                                       "observed-success": 0, "source-read": errno.EIO, "source-eof": errno.EIO,
                                       "copy-late-code": errno.EBADMSG, "copy-data": errno.ESTALE,
                                       "verify-data": errno.EBADMSG, "bootable-restore": 0}
                fixtures = json.loads((base / 'initramfs/lzw-fixtures/manifest.json').read_text())
                expected_compressed.update({row['name']: row['error'] for row in fixtures['cases']})
                expected_compressed = {"lzw-" + name: error for name, error in expected_compressed.items()}
                assert len(expected_compressed) == 36 and set(compressed) == set(expected_compressed), compressed
                assert all(compressed[name]["ERROR"] == error for name, error in expected_compressed.items()), compressed
                claims = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_CLAIM_CASE (\S+) ([^\r\n]+)", text)}
                expected_claims = {"lzw-" + name for name in ("write", "cancel", "sync", "verify", "close",
                                   "observed-success", "source-read", "source-eof", "copy-late-code", "copy-data", "verify-data")}
                assert set(claims) == expected_claims, claims
                assert all(report["SYNCS"] == 1 and report["CLOSES"] == 1 and report["CLAIMS"] >= 5 for report in claims.values()), claims
                for name in ("lzw-source-read", "lzw-source-eof"):
                    assert claims[name]["SOURCE_READS"] >= 4 and compressed[name]["WRITTEN"] == 65536, (name, claims, compressed)
                assert compressed['lzw-copy-late-code']['WRITTEN'] == compressed['lzw-copy-data']['WRITTEN'] == 1024 * 1024
                assert compressed['lzw-verify-data']['VERIFIED'] == fixtures['decoded_flip_index'] // 65536 * 65536
                assert "RUFUS_LZW_HELPER_DONE SHA256=" in text and "RUFUS_COMPRESSED_QT_DONE" not in text, serial
                (base / 'compressed-helper-results.json').write_text(json.dumps({'cases': compressed, 'claims': claims}, indent=2) + '\n')
            if args.zip_raw_helper:
                expected_raw |= {"zip-" + name for name in ("busy", "home-target", "raw-cancel-after", "raw-success")}
                compressed = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_HELPER_CASE (\S+) ([^\r\n]+)", text)}
                expected_compressed = {"digest": errno.ESTALE, "size": errno.ESTALE, "sector": errno.ESTALE,
                                       "crc": errno.EBADMSG, "write": errno.EIO, "cancel": errno.ECANCELED,
                                       "sync": errno.EIO, "verify": errno.EBADMSG, "close": errno.EIO,
                                       "observed-success": 0, "source-read": errno.EIO, "source-eof": errno.EIO,
                                       "bootable-restore": 0}
                for method in (0, 8, 12, 14, 95):
                    expected_compressed[f"method{method}-success"] = 0
                    expected_compressed[f"method{method}-crc"] = errno.EBADMSG
                expected_compressed = {"zip-" + name: error for name, error in expected_compressed.items()}
                assert len(expected_compressed) == 23 and set(compressed) == set(expected_compressed), compressed
                assert all(compressed[name]["ERROR"] == error for name, error in expected_compressed.items()), compressed
                claims = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_CLAIM_CASE (\S+) ([^\r\n]+)", text)}
                expected_claims = {"zip-" + name for name in
                                   ("write", "cancel", "sync", "verify", "close", "observed-success", "source-read", "source-eof")}
                assert set(claims) == expected_claims, claims
                assert all(report["SYNCS"] == report["CLOSES"] == 1 and report["CLAIMS"] >= 5 for report in claims.values()), claims
                for name in ("zip-source-read", "zip-source-eof"):
                    assert claims[name]["SOURCE_READS"] >= 4 and compressed[name]["WRITTEN"] == 65536, (name, claims, compressed)
                assert "RUFUS_ZIP_HELPER_DONE SHA256=" in text and "RUFUS_COMPRESSED_QT_DONE" not in text, serial
                (base / 'compressed-helper-results.json').write_text(json.dumps({'cases': compressed, 'claims': claims}, indent=2) + '\n')
            if args.zstd_raw_helper:
                expected_raw |= {"zstd-" + name for name in ("busy", "home-target", "raw-cancel-after", "raw-success")}
                compressed = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_HELPER_CASE (\S+) ([^\r\n]+)", text)}
                expected_compressed = {"digest": errno.ESTALE, "size": errno.ESTALE, "sector": errno.ESTALE,
                                       "crc": errno.EBADMSG, "write": errno.EIO, "cancel": errno.ECANCELED,
                                       "sync": errno.EIO, "verify": errno.EBADMSG, "close": errno.EIO,
                                       "observed-success": 0, "source-read": errno.EIO, "source-eof": errno.EIO,
                                       "bootable-restore": 0}
                expected_compressed = {"zstd-" + name: error for name, error in expected_compressed.items()}
                assert set(compressed) == set(expected_compressed), compressed
                assert all(compressed[name]["ERROR"] == error for name, error in expected_compressed.items()), compressed
                claims = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_CLAIM_CASE (\S+) ([^\r\n]+)", text)}
                expected_claims = {"zstd-" + name for name in
                                   ("write", "cancel", "sync", "verify", "close", "observed-success", "source-read", "source-eof")}
                assert set(claims) == expected_claims, claims
                assert all(report["SYNCS"] == report["CLOSES"] == 1 and report["CLAIMS"] >= 5 for report in claims.values()), claims
                assert "RUFUS_ZSTD_HELPER_DONE SHA256=" in text and "RUFUS_COMPRESSED_QT_DONE" not in text, serial
                (base / 'compressed-helper-results.json').write_text(json.dumps({'cases': compressed, 'claims': claims}, indent=2) + '\n')
            if args.vtsi_raw_helper:
                compressed = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_HELPER_CASE (\S+) ([^\r\n]+)", text)}
                expected_vtsi = {"vtsi-layout": errno.ESTALE}
                if not args.vtsi_qt_helper:
                    expected_vtsi.update({"vtsi-observed-sparse": 0, "vtsi-bootable-restore": 0})
                assert set(compressed) == set(expected_vtsi), compressed
                assert all(compressed[name]["ERROR"] == error for name, error in expected_vtsi.items()), compressed
                if not args.vtsi_qt_helper:
                    assert compressed["vtsi-observed-sparse"]["WRITTEN"] == 128 * 1024, compressed
                    assert compressed["vtsi-observed-sparse"]["VERIFIED"] == 128 * 1024, compressed
                    assert compressed["vtsi-bootable-restore"]["WRITTEN"] == HANDOFF_DISK_BYTES, compressed
                    assert compressed["vtsi-bootable-restore"]["VERIFIED"] == HANDOFF_DISK_BYTES, compressed
                claims = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_CLAIM_CASE (\S+) ([^\r\n]+)", text)}
                assert set(claims) == (set() if args.vtsi_qt_helper else {"vtsi-observed-sparse"}), claims
                if not args.vtsi_qt_helper:
                    assert claims["vtsi-observed-sparse"]["WRITES"] >= 2, claims
                    assert claims["vtsi-observed-sparse"]["READS"] >= 2, claims
                    assert claims["vtsi-observed-sparse"]["CLAIMS"] >= 5, claims
                    assert claims["vtsi-observed-sparse"]["SYNCS"] == claims["vtsi-observed-sparse"]["CLOSES"] == 1, claims
                assert "RUFUS_VTSI_HELPER_DONE SHA256=" in text, serial
                (base / 'vtsi-helper-results.json').write_text(json.dumps({'cases': compressed, 'claims': claims}, indent=2) + '\n')
            if args.vhd_raw_helper:
                compressed = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_HELPER_CASE (\S+) ([^\r\n]+)", text)}
                expected_vhd = {"vhd-sha": (errno.ESTALE, 0, 0)}
                if not args.vhd_qt_helper:
                    expected_vhd.update({"vhd-observed-dynamic": (0, 256 * 1024, 256 * 1024),
                                         "vhd-bootable-restore": (0, HANDOFF_DISK_BYTES, HANDOFF_DISK_BYTES)})
                assert set(compressed) == set(expected_vhd), compressed
                assert all((compressed[name]["ERROR"], compressed[name]["WRITTEN"], compressed[name]["VERIFIED"]) == counts
                           for name, counts in expected_vhd.items()), compressed
                claims = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_CLAIM_CASE (\S+) ([^\r\n]+)", text)}
                assert set(claims) == (set() if args.vhd_qt_helper else {"vhd-observed-dynamic"}), claims
                if not args.vhd_qt_helper:
                    assert claims["vhd-observed-dynamic"]["WRITES"] >= 2 and claims["vhd-observed-dynamic"]["READS"] >= 2, claims
                    assert claims["vhd-observed-dynamic"]["CLAIMS"] >= 5 and claims["vhd-observed-dynamic"]["SYNCS"] == claims["vhd-observed-dynamic"]["CLOSES"] == 1, claims
                assert "RUFUS_VHD_HELPER_DONE SHA256=" in text, serial
                (base / 'vhd-helper-results.json').write_text(json.dumps({'cases': compressed, 'claims': claims}, indent=2) + '\n')
            if args.media_appearance:
                cases = re.findall(r"RUFUS_APPEARANCE_CASE FS=(\d+) SFD=(\d+) READBACK=1 AUTH=1 JOINED=1", text)
                assert cases == [(str(fs), str(sfd)) for fs in (0, 1, 2, 4) for sfd in (0, 1)], cases
                assert 'RUFUS_APPEARANCE_PRESERVED ISO=1 CASE=1 ICON=1' in text
                assert 'RUFUS_APPEARANCE_QT_DONE AUTH=1 JOINED=1 READY=100 DEFAULT=1' in text
                jobs = [values(frame) for frame in re.findall(r"RUFUS_COMPRESSED_QT_DONE ([^\r\n]+)", text)]
                assert len(jobs) == 1 and jobs[0]['WRITTEN'] == jobs[0]['VERIFIED'] == HANDOFF_DISK_BYTES
                assert jobs[0]['HELD'] == jobs[0]['LAUNCHES'] == 1 and jobs[0]['PERCENT'] == 100
                (base / 'appearance-results.json').write_text(json.dumps({'cases': cases, 'preserved': True, 'default_qt': True, 'restore': jobs[0]}, indent=2) + '\n')
            elif args.bad_blocks:
                cases = re.findall(r"RUFUS_BADBLOCK_CASE CHOICE=(\d+) BAD=120 JOINED=1 AUTH=1", text)
                assert cases == ['0', '1', '2'], cases
                frames = re.findall(r"RUFUS_COMPRESSED_QT_DONE ([^\r\n]+)", text)
                jobs = [values(frame) for frame in frames]
                assert len(jobs) == 1 and jobs[0]['WRITTEN'] == jobs[0]['VERIFIED'] == HANDOFF_DISK_BYTES
                assert jobs[0]['HELD'] == jobs[0]['LAUNCHES'] == 1 and jobs[0]['PERCENT'] == 100
                (base / 'badblocks-results.json').write_text(json.dumps({'cases': cases, 'restore': jobs[0]}, indent=2) + '\n')
            elif args.old_bios:
                cases = re.findall(r"RUFUS_OLD_BIOS_CASE FS=(\d+) OFFSET=(\d+) READBACK=1 HELPER=(\d+)", text)
                assert [int(row[0]) for row in cases] == [6, 7, 0, 2, 1], cases
                assert "RUFUS_OLD_BIOS_QT_DONE AUTH=1 JOINED=1 READY=100 FROZEN=1" in text
                frames = re.findall(r"RUFUS_COMPRESSED_QT_DONE ([^\r\n]+)", text)
                jobs = [values(frame) for frame in frames]
                assert len(jobs) == 1 and jobs[0]['WRITTEN'] == jobs[0]['VERIFIED'] == HANDOFF_DISK_BYTES
                assert jobs[0]['HELD'] == jobs[0]['LAUNCHES'] == 1 and jobs[0]['PERCENT'] == 100
                (base / 'old-bios-results.json').write_text(json.dumps({'cases': cases, 'restore': jobs[0]}, indent=2) + '\n')
            elif args.full_format:
                formats = re.findall(r"RUFUS_FULL_FORMAT_CASE FS=(\d+) FULL=1 READBACK=1 HELPER=(\d+) JOINED=1", text)
                assert len(formats) == len(set(row[0] for row in formats)) == 7, formats
                assert [row[1] for row in formats] == ['1'] * 4 + ['0'] * 3, formats
                frames = re.findall(r"RUFUS_COMPRESSED_QT_DONE ([^\r\n]+)", text)
                jobs = [values(frame) for frame in frames]
                assert len(jobs) == 1 and jobs[0]['WRITTEN'] == jobs[0]['VERIFIED'] == HANDOFF_DISK_BYTES
                assert jobs[0]['HELD'] == jobs[0]['LAUNCHES'] == 1 and jobs[0]['PERCENT'] == 100
                (base / 'full-format-results.json').write_text(json.dumps({'formats': formats, 'restore': jobs[0]}, indent=2) + '\n')
            elif args.zero_device:
                frames = re.findall(r"RUFUS_COMPRESSED_QT_DONE ([^\r\n]+)", text)
                qt = [values(frame) for frame in frames]
                assert [row['ZERO'] for row in qt] == [2, 2, 1, 0], qt
                expected = [32 * MIB, 0, HANDOFF_DISK_BYTES, HANDOFF_DISK_BYTES]
                assert [row['WRITTEN'] for row in qt] == [row['VERIFIED'] for row in qt] == expected, qt
                assert all(row['HELD'] == row['LAUNCHES'] == 1 and row['PERCENT'] == 100 for row in qt), qt
                assert all('HELD99=1' in frame for frame in frames), frames
                assert 'RUFUS_ZERO_DEVICE_DONE SHA256=' in text
                (base / 'zero-device-results.json').write_text(json.dumps(qt, indent=2) + '\n')
            elif args.image_entry:
                frames = re.findall(r"RUFUS_COMPRESSED_QT_DONE ([^\r\n]+)", text)
                qt = [values(frame) for frame in frames]
                assert [row['TYPE'] for row in qt] == [0, 3, 0], qt
                assert all(row['WRITTEN'] == row['VERIFIED'] == HANDOFF_DISK_BYTES for row in qt), qt
                assert all(row['HELD'] == row['LAUNCHES'] == 1 and row['PERCENT'] == 100 for row in qt), qt
                assert all('HELD99=1' in frame for frame in frames), frames
                for name in ('raw-marker-required', 'gzip-marker-required'):
                    refusal = re.search('RUFUS_COMPRESSED_HELPER_CASE ' + name + r' ([^\r\n]+)', text)
                    assert refusal and values(refusal[1])['ERROR'] == errno.ENOTSUP
                    assert values(refusal[1])['WRITTEN'] == values(refusal[1])['VERIFIED'] == 0
                assert 'RUFUS_IMAGE_ENTRY_DONE SHA256=' in text
                (base / 'image-entry-results.json').write_text(json.dumps(qt, indent=2) + '\n')
            elif args.ffu_qt_helper:
                qt_frames = re.findall(r"RUFUS_COMPRESSED_QT_DONE ([^\r\n]+)", text)
                qt = [values(report) for report in qt_frames]
                sizes = ([80 * MIB, 56 * MIB] if args.ffu_optimized else [262144, 393216] if args.ffu_stores else [HANDOFF_DISK_BYTES] if args.ffu_partial else [8 * MIB + 1536] if args.ffu_geometry else [MIB, MIB, 8 * MIB] if args.ffu_variants else [MIB]) + [HANDOFF_DISK_BYTES]
                assert len(qt) == len(sizes) and all(report["TYPE"] == 10 for report in qt), qt
                assert [report["DECODED"] for report in qt] == sizes, qt
                assert all(0 < report["WRITTEN"] <= HANDOFF_DISK_BYTES and report["VERIFIED"] == report["WRITTEN"] for report in qt), qt
                if args.ffu_optimized:
                    cases = re.findall(r"RUFUS_FFU_OPTIMIZED_CASE NAME=(smaller|larger) NTFS_GROWN=1 RECOVERY_MOVED=1 XML_READBACK=1 GPT_VALID=1 SOURCE_UNCHANGED=1", text)
                    assert cases == ["smaller", "larger"], cases
                if args.ffu_stores:
                    cases = re.findall(r"RUFUS_FFU_STORE_CASE INDEX=(\d+) BYTES=(\d+) READBACK=1 SOURCE_UNCHANGED=1", text)
                    assert cases == [("1", "262144"), ("2", "393216")], cases
                if args.ffu_partial:
                    assert [report['WRITTEN'] for report in qt] == [HANDOFF_DISK_BYTES, 131072]
                    failed = re.search(r"RUFUS_COMPRESSED_HELPER_CASE ffu-base-mismatch ([^\r\n]+)", text)
                    assert failed and values(failed[1])['ERROR'] == errno.ESTALE
                    assert values(failed[1])['WRITTEN'] == values(failed[1])['VERIFIED'] == 0
                if args.ffu_variants: assert "RUFUS_FFU_GPT_ENLARGED PARTITION_TABLE_UNCHANGED=1 HEADERS_VALID=1" in text
                if args.ffu_geometry:
                    captured = re.search(r"RUFUS_FFU_CAPTURE_EXACT SOURCE_BYTES=(\d+) BLOCK_BYTES=131072 READ_ONLY=1 SHA256=([0-9a-f]{64})", text)
                    original = json.loads((base / 'ffu-source.json').read_text())
                    assert captured and int(captured[1]) == original['bytes'] == 8 * MIB + 1536
                    assert captured[2] == original['sha256'] == digest(base / 'ffu-source.raw')
                    trailing = 2 * MIB + 512 if args.ffu_layouts else 131072
                    assert qt[0]['WRITTEN'] == 8 * MIB + trailing and qt[1]['WRITTEN'] == HANDOFF_DISK_BYTES
                    if args.ffu_layouts:
                        layout = re.search(r'RUFUS_FFU_CAPTURE_LAYOUT MANIFEST_BYTES=(\d+) TABLE_BYTES=(\d+) MULTI_CHUNK=1', text)
                        assert layout and int(layout[1]) > 131072 and int(layout[2]) == 2 * MIB

                held99 = [re.search(r"\bHELD99=(\d+)\b", frame) for frame in qt_frames]
                assert all(marker and report["SECTOR"] == 512 and report["HELD"] == int(marker[1]) == report["LAUNCHES"] == 1
                           and report["PERCENT"] == 100 for report, marker in zip(qt, held99)), qt
                marker = "RUFUS_FFU_OPTIMIZED_DONE SHA256=" if args.ffu_optimized else "RUFUS_FFU_STORES_DONE SHA256=" if args.ffu_stores else "RUFUS_FFU_PARTIAL_DONE SHA256=" if args.ffu_partial else "RUFUS_FFU_GEOMETRY_DONE SHA256=" if args.ffu_geometry else "RUFUS_FFU_VARIANTS_DONE SHA256=" if args.ffu_variants else "RUFUS_FFU_QT_HELPER_DONE SHA256="
                assert marker in text, serial
                (base / 'ffu-qt-results.json').write_text(json.dumps(qt, indent=2) + '\n')
            if args.vhdx_qt_helper:
                qt_frames = re.findall(r"RUFUS_COMPRESSED_QT_DONE ([^\r\n]+)", text)
                qt = [values(report) for report in qt_frames]
                assert [report["TYPE"] for report in qt] == ([11, 12, 12] if args.virtual_disk_chains else [12, 12]), qt
                sizes = ([262144] if args.virtual_disk_chains else []) + [3 * MIB, HANDOFF_DISK_BYTES]
                assert [report["DECODED"] for report in qt] == sizes, qt
                assert [(report["DECODED"], report["WRITTEN"], report["VERIFIED"]) for report in qt] == [(size, size, size) for size in sizes], qt
                held99 = [re.search(r"\bHELD99=(\d+)\b", frame) for frame in qt_frames]
                assert all(marker and report["SECTOR"] == 512 and report["HELD"] == int(marker[1]) == report["LAUNCHES"] == 1
                           and report["PERCENT"] == 100 for report, marker in zip(qt, held99)), qt
                marker = "RUFUS_VIRTUAL_DISK_DONE SHA256=" if args.virtual_disk_chains else "RUFUS_VHDX_QT_HELPER_DONE SHA256="
                assert marker in text, serial
                if args.virtual_disk_chains:
                    cases = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_HELPER_CASE (\S+) ([^\r\n]+)", text)}
                    assert set(cases) == {"vhd-parent-owner", "vhd-parent-target"}, cases
                    assert cases["vhd-parent-owner"]["ERROR"] == errno.EACCES and cases["vhd-parent-target"]["ERROR"] == errno.EBUSY, cases
                    assert all(case["WRITTEN"] == case["VERIFIED"] == 0 for case in cases.values()), cases
                    (base / 'chain-protection-results.json').write_text(json.dumps(cases, indent=2) + '\n')
                (base / 'vhdx-qt-results.json').write_text(json.dumps(qt, indent=2) + '\n')
            if args.vhd_qt_helper:
                qt_frames = re.findall(r"RUFUS_COMPRESSED_QT_DONE ([^\r\n]+)", text)
                qt = [values(report) for report in qt_frames]
                assert len(qt) == 2 and all(report["TYPE"] == 11 for report in qt), qt
                assert [(report["DECODED"], report["WRITTEN"], report["VERIFIED"]) for report in qt] == [
                    (256 * 1024, 256 * 1024, 256 * 1024),
                    (HANDOFF_DISK_BYTES, HANDOFF_DISK_BYTES, HANDOFF_DISK_BYTES)], qt
                assert all(report["STORED"] != report["DECODED"] for report in qt), qt
                held99 = [re.search(r"\bHELD99=(\d+)\b", frame) for frame in qt_frames]
                assert all(marker and report["SECTOR"] == 512 and report["HELD"] == int(marker[1]) == report["LAUNCHES"] == 1
                           and report["PERCENT"] == 100 for report, marker in zip(qt, held99)), qt
                assert "RUFUS_VHD_QT_HELPER_DONE SHA256=" in text, serial
                (base / 'vhd-qt-results.json').write_text(json.dumps(qt, indent=2) + '\n')
            if args.vtsi_qt_helper:
                qt_frames = re.findall(r"RUFUS_COMPRESSED_QT_DONE ([^\r\n]+)", text)
                qt = [values(report) for report in qt_frames]
                assert len(qt) == 2 and all(report["TYPE"] == 8 for report in qt), qt
                assert [(report["DECODED"], report["WRITTEN"], report["VERIFIED"]) for report in qt] == [
                    (512 * 512, 128 * 1024, 128 * 1024),
                    (HANDOFF_DISK_BYTES, HANDOFF_DISK_BYTES, HANDOFF_DISK_BYTES)], qt
                held99 = [re.search(r"\bHELD99=(\d+)\b", frame) for frame in qt_frames]
                assert all(marker and (report["SECTOR"], report["HELD"], int(marker[1]),
                            report["PERCENT"], report["LAUNCHES"]) == (512, 1, 1, 100, 1)
                           for report, marker in zip(qt, held99)), qt
                assert "RUFUS_VTSI_QT_HELPER_DONE SHA256=" in text, serial
                (base / 'vtsi-qt-results.json').write_text(json.dumps(qt, indent=2) + '\n')
            if args.bzip2_qt_helper or args.lzma_qt_helper or args.zip_qt_helper or args.zstd_qt_helper:
                qt_frames = re.findall(r"RUFUS_COMPRESSED_QT_DONE ([^\r\n]+)", text)
                qt = {values(report)["TYPE"]: values(report) for report in qt_frames}
                expected_type = 9 if args.zstd_qt_helper else 1 if args.zip_qt_helper else 4 if args.lzma_qt_helper else 5
                name = 'ZSTD' if args.zstd_qt_helper else 'ZIP' if args.zip_qt_helper else 'LZMA' if args.lzma_qt_helper else 'BZIP2'
                assert len(qt_frames) == 1 and set(qt) == {expected_type}, qt
                assert qt[expected_type]["WRITTEN"] == qt[expected_type]["VERIFIED"] == HANDOFF_DISK_BYTES, qt
                if args.zip_qt_helper or args.zstd_qt_helper:
                    held99 = re.search(r"\bHELD99=(\d+)\b", qt_frames[0])
                    assert held99, qt_frames
                    assert (qt[expected_type]["SECTOR"], qt[expected_type]["HELD"],
                            int(held99[1]), qt[expected_type]["PERCENT"],
                            qt[expected_type]["LAUNCHES"]) == (512, 1, 1, 100, 1), qt
                assert f"RUFUS_{name}_QT_HELPER_DONE SHA256=" in text, serial
                (base / "compressed-qt-results.json").write_text(json.dumps(qt, indent=2) + "\n")
            if args.lzw_qt_helper:
                qt = {}
                qt_frames = re.findall(r"RUFUS_COMPRESSED_QT_DONE ([^\r\n]+)", text)
                for report in qt_frames:
                    row = {key: int(value) for key, value in re.findall(r"\b([A-Z][A-Z0-9_]*)=(\d+)\b", report)}
                    qt[row['TYPE']] = row
                assert len(qt_frames) == 2 and set(qt) == {2, 3}, qt
                assert all(row['STORED'] != row['DECODED'] == row['WRITTEN'] == row['VERIFIED'] == HANDOFF_DISK_BYTES
                           and row['SECTOR'] == 512 and row['LAUNCHES'] == row['HELD'] == row['HELD99'] == 1
                           and row['PERCENT'] == 100
                           for row in qt.values()), qt
                direct = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_HELPER_CASE (\S+) ([^\r\n]+)", text)}
                expected = {'source-read': (errno.EIO, 65536, 0), 'source-eof': (errno.EIO, 65536, 0),
                            'copy-late-code': (errno.EBADMSG, 1048576, 0), 'copy-data': (errno.ESTALE, 1048576, 0),
                            'verify-data': (errno.EBADMSG, 1049088, 524288)}
                assert set(direct) == {'lzw-gzip-' + name for name in expected}, direct
                for name, counts in expected.items():
                    row = direct['lzw-gzip-' + name]
                    assert (row['ERROR'], row['WRITTEN'], row['VERIFIED']) == counts and row['SYNC'] == 0, row
                claims = {name: values(report) for name, report in re.findall(r"RUFUS_COMPRESSED_CLAIM_CASE (\S+) ([^\r\n]+)", text)}
                assert set(claims) == set(direct) and all(row['SYNCS'] == row['CLOSES'] == 1 and row['CLAIMS'] >= 5 for row in claims.values()), claims
                assert 'RUFUS_LZW_QT_HELPER_DONE SHA256=' in text, serial
                (base / 'compressed-qt-results.json').write_text(json.dumps(qt, indent=2) + '\n')
                (base / 'compressed-helper-results.json').write_text(json.dumps({'cases': direct, 'claims': claims}, indent=2) + '\n')
            if args.ntfs_mounted_helper:
                expected_raw |= {driver + '-' + case for driver in ('ntfs-3g', 'lowntfs-3g')
                                 for case in ('raw-source-changed', 'busy', 'home-target', 'mount-mismatch',
                                              'raw-cancel-after', 'format-success', 'raw-success')}
                assert 'RUFUS_NTFS_MOUNT_HELPER_DONE' in text, serial
            if args.fixed_usb_helper:
                expected_raw |= {"fixed-format-unmounted", "fixed-raw-unmounted"}
                fixed = {name: values(report) for name, report in re.findall(r"RUFUS_FIXED_USB_CASE (\S+) ([^\r\n]+)", text)}
                assert set(fixed) == {prefix + "-" + case for prefix in ("default", "opt-in")
                                      for case in ("hidden-mount", "home-target", "source-target", "system-target", "holder", "success")}, fixed
                (base / "fixed-usb-results.json").write_text(json.dumps(fixed, indent=2) + "\n")
            assert set(raw) == expected_raw, raw
            assert "RUFUS_RAW_HELPER_DONE" in text, serial
            (base / "raw-helper-results.json").write_text(json.dumps(raw, indent=2) + "\n")
        if args.mounted_raw:
            raw = {name: values(report) for name, report in re.findall(r"RUFUS_RAW_DEVICE_CASE (\S+) ([^\r\n]+)", text)}
            expected_raw = {"source-changed", "wrong-owner", "bad-marker", "wim", "partial-sector", "too-large",
                            "wrong-sequence", "busy", "cancel-before", "cancel-unmount", "cancel-copy", "sync-fail", "verify-fail", "success"}
            assert set(raw) == expected_raw and "RUFUS_RAW_DEVICE_DONE" in text, raw
            (base / "raw-results.json").write_text(json.dumps(raw, indent=2) + "\n")
        return reports
    if args.install:
        for marker in ("RUFUS_INSTALL_DONE", "RUFUS_MBR_EXIT 0", "RUFUS_INSTALL_CHILD_CLAIM_OK"):
            assert marker in text, (marker, serial)
        reports = {name: values(report) for name, report in re.findall(r"RUFUS_INSTALL_CASE (\S+) ([^\r\n]+)", text)}
        expected = {"file-only-refused", "wrong-device", "wrong-capacity", "wrong-sequence", "wrong-extent",
                    "wrong-volume-id", "wrong-filesystem", "mbr-changed", "cancel-after", "sync-fail", "success"}
        assert set(reports) == expected, reports
        return reports
    if args.exfat:
        assert "RUFUS_EXFAT_DONE" in text and "RUFUS_MBR_EXIT 0" in text, serial
        reports = {name: values(report) for name, report in re.findall(r"RUFUS_EXFAT_CASE (\S+) ([^\r\n]+)", text)}
        expected = {"zero-device", "wrong-device", "wrong-capacity", "wrong-offset", "unaligned-offset",
                    "readonly-fd", "readonly-device", "whole-disk", "file-api-refused", "metadata-space", "held-claim",
                    "snapshot-fail", "gpt-primary-changed", "gpt-backup-changed", "busy-reacquire",
                    "cancel-after", "sync-fail", "success"}
        if sector != 512:
            expected = {"unsupported-sector"}
        assert set(reports) == expected, reports
        return reports
    if args.handoff:
        for marker in ("RUFUS_HANDOFF_DONE", "RUFUS_MBR_EXIT 0", "RUFUS_HANDOFF_FILESYSTEM_TAIL_CHANGED"):
            assert marker in text, (marker, serial)
        reports = {name: values(report) for name, report in re.findall(r"RUFUS_HANDOFF_CASE (\S+) ([^\r\n]+)", text)}
        expected = {"ntfs-refused", "snapshot-fail", "format-mbr-changed", "format-busy", "format-cancel",
                    "gpt-primary-changed", "gpt-backup-changed", "gpt-success", "before-mount-changed",
                    "mount-fail", "sync-fail", "mounted-mbr-changed", "mbr-fill"}
        assert set(reports) == expected, reports
        return reports
    for marker in ("RUFUS_MBR_DONE", "RUFUS_MBR_EXIT 0", f"SECTOR={sector} CAPACITY={DISK_BYTES}"):
        assert marker in text, (marker, serial)
    reports = {name: values(report) for name, report in re.findall(r"RUFUS_MBR_CASE (\S+) ([^\r\n]+)", text)}
    expected = {"missing-device", "wrong-device", "wrong-capacity", "cancel-before",
                "native-readonly", "readonly-descriptor", "success"} if sector == 512 else {"native-4k"}
    assert set(reports) == expected, reports
    if sector == 512:
        assert "RUFUS_MBR_KERNEL_LAYOUT_AND_CLAIM_OK" in text, serial
    return reports


def inspect_freedos(args, base, disk):
    from test_freedos_boot import expected_records
    from test_freedos_copy import PAYLOADS
    from test_freedos_startup import reference
    import hashlib

    reports = re.findall(r"RUFUS_FREEDOS_VOLUME ([^\r\n]+)", (base / "serial-512.log").read_text())
    assert len(reports) == 1, reports
    report = values(reports[0])
    offset, size = report["OFFSET"], report["SIZE"]
    cluster = FAT16_CLUSTER_BYTES if args.fat_bits == 16 else SECTOR_BYTES
    bootstrap = "win7" if args.freedos_win7 else "rufus"
    with disk.open("rb") as stream:
        mbr = stream.read(512)
        stream.seek(offset)
        records = stream.read(9 * 512)
    signature = struct.unpack_from("<I", mbr, 440)[0]
    inspect_mbr(disk, HANDOFF_DISK_BYTES, cluster=cluster, signature=signature, file_system=args.fat_bits, boot_code=bootstrap)
    assert records == expected_records(records, args.fat_bits), "Original FreeDOS PBR bytes"
    if args.fat_bits == 32:
        assert records[:512] == records[6 * 512:7 * 512], "FAT32 backup boot sector"
    keyboard, cp = ("us", 437) if args.freedos_win7 else ("ro", 852)
    startup = reference(argparse.Namespace(reference=str(args.build / "dos_locale_reference")), base, keyboard, cp)
    expected = dict(PAYLOADS, **startup)
    volume = base / "volume.raw"
    copy_volume(disk, volume, offset, size)
    check_fat_volume(base, volume, standard=True, log="freedos-fsck.log")
    attrs = run(["mattrib", "-i", str(volume), "::/*", "::/LOCALE/*"], capture_output=True, text=True).stdout
    (base / "freedos-attributes.log").write_text(attrs)
    listing = run(["mdir", "-a", "-i", str(volume), "::/", "::/LOCALE"], capture_output=True).stdout
    (base / "freedos-listing.log").write_bytes(listing)
    files = {}
    for name, data in expected.items():
        actual = run(["mtype", "-i", str(volume), "::/" + name], capture_output=True).stdout
        assert actual == data, name
        line = next(line for line in attrs.splitlines() if line.endswith("::/" + name))
        flags = line.split("::", 1)[0]
        system = name in ("KERNEL.SYS", "COMMAND.COM")
        assert ("H" in flags) == system and ("S" in flags) == system, line
        files[name] = {"bytes": len(data), "sha256": hashlib.sha256(actual).hexdigest(), "attributes": flags.strip()}
    names = {line.split("::/", 1)[1] for line in attrs.splitlines() if "::/" in line}
    assert names == set(expected) | {"LOCALE", "LOCALE/.", "LOCALE/.."}, names
    if args.freedos_win7:
        assert "CONFIG.SYS" not in attrs, attrs
    assert sum(len(data) for data in expected.values()) == report["COPIED"], report
    for path in base.glob("oracle*\\*"):
        path.unlink()
    volume.unlink()
    return {"bootstrap": bootstrap, "keyboard": keyboard, "codepage": cp, "signature": signature,
            "offset": offset, "size": size, "files": files, "sha256": digest(disk)}


def inspect_handoff(base, disk, fat_bits):
    cluster = FAT16_CLUSTER_BYTES if fat_bits == 16 else 512
    inspect_mbr(disk, HANDOFF_DISK_BYTES, cluster=cluster, file_system=fat_bits)
    offset, size = extent(HANDOFF_DISK_BYTES, cluster)
    volume, copied = base / "volume.raw", base / "filled.bin"
    copy_volume(disk, volume, offset, size)
    checked = subprocess.run(["fsck.fat", "-n", str(volume)], capture_output=True, timeout=20)
    (base / "fsck.log").write_bytes(checked.stdout + checked.stderr)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    run(["mcopy", "-i", f"{disk}@@{offset}", "::/filled.bin", str(copied)], capture_output=True)
    serial = (base / "serial-512.log").read_text()
    count = int(re.search(r"RUFUS_HANDOFF_FILLED_BYTES=(\d+)", serial)[1])
    assert copied.stat().st_size == count
    with copied.open("rb") as source:
        while data := source.read(MIB):
            assert data == b"K" * len(data), "Filesystem content differs after sync/unmount"
    with disk.open("rb") as source:
        source.seek(HANDOFF_DISK_BYTES - GPT_BACKUP_BYTES)
        tail = source.read()
        assert b"K" * 512 in tail, "Fixture did not reach the former GPT backup area"
        if fat_bits == 32:
            assert tail[-512:] == b"K" * 512, "FAT32 fixture did not reach the final sector"
    return {"copied_bytes": count, "file_sha256": digest(copied), "disk_sha256": digest(disk)}


def make_install_fixture(args, base):
    sys.path.append(str(prepare.HERE.parents[1] / "probes/syslinux"))
    from test_layout import make_fixture
    fixture = base / "fixture"
    fixture.mkdir()
    args.iso, tree, unused_prefix = make_fixture(args.upstream, fixture,
        "/isolinux/isolinux.cfg", False, False, os.environ.copy(), "")
    expected = base / "expected"
    shutil.copytree(tree, expected)
    (expected / "ldlinux.sys").unlink()
    if args.helper:
        for name in ("unknown", "incompatible"):
            directory = base / name
            directory.mkdir()
            image, unknown_tree, prefix = make_fixture(args.upstream, directory,
                "/isolinux/isolinux.cfg", False, True, os.environ.copy(), "")
            if name == "unknown":
                shutil.copy2(unknown_tree / "isolinux/isolinux.bin", expected / "isolinux/isolinux.bin")
            else:
                # Keep a valid Syslinux image but supply a mismatched module.
                module = unknown_tree / "isolinux/ldlinux.c32"
                damaged = bytearray(module.read_bytes())
                damaged[-1] ^= 1
                module.write_bytes(damaged)
                image.unlink()
                run(["bsdtar", "--format=iso9660", "-cf", image, "-C", unknown_tree, "."], capture_output=True)
            image.rename(directory / "source.iso")
    if args.media_appearance:
        appearance = base / "appearance-tree"
        shutil.copytree(tree, appearance)
        metadata = b'\xff\xfe' + '; image-owned\r\n[autorun]\r\nlabel=KEEP THIS\r\n'.encode('utf-16le')
        (appearance / 'AUTORUN.INF').write_bytes(metadata)
        (appearance / 'autorun.ico').write_bytes(b'custom icon bytes')
        (base / 'expected-autorun.inf').write_bytes(metadata)
        run(['bsdtar', '--format=iso9660', '-cf', base / 'appearance.iso', '-C', appearance, '.'], capture_output=True)
    (expected / "syslinux.cfg").write_text("DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG /isolinux/isolinux.cfg\n  APPEND /isolinux/\n")


def make_ntfs_image_fixture(args, base):
    from syslinux_ntfs import make_iso, make_image
    args.iso = make_iso(args, base)
    tree = base / "RUFUS_SOURCE"
    entries = {path.relative_to(tree).as_posix(): path.read_bytes() for path in tree.rglob('*') if path.is_file()}
    if args.syslinux_uefi:
        make_image(base, "bios", entries, args.mkisofs)
        entries["EFI/BOOT/BOOTX64.EFI"] = (args.build / "uefi-handoff.efi").read_bytes()
        shutil.rmtree(tree)
        args.iso = make_image(base, "RUFUS_SOURCE", entries, args.mkisofs)
        target = base / "iso-expected/EFI/BOOT/BOOTX64.EFI"
        target.parent.mkdir(parents=True)
        target.write_bytes(entries["EFI/BOOT/BOOTX64.EFI"])
    for name in ("unknown", "incompatible", "conflict", "efi"):
        variant = entries.copy()
        if name == "unknown":
            variant["boot/x86_64/loader/isolinux.bin"] = bytes(512)
        elif name == "incompatible":
            module = bytearray(variant["ldlinux.c32"])
            module[-1] ^= 1
            variant["ldlinux.c32"] = module
        elif name == "conflict":
            variant["extlinux.conf"] = b"DEFAULT conflicting-root-config\n"
            variant["boot/x86_64/loader/syslinux.cfg"] = b"DEFAULT selected-directory-config\n"
        else:
            variant["EFI/BOOT/BOOTX64.EFI"] = b"MZ" + bytes(510)
        make_image(base, name, variant, args.mkisofs)
    # Check fixture policy before paying for a whole native guest. These are
    # the same preflight entry point and retained-source checks as the helper.
    reports = {}
    cases = [('RUFUS_SOURCE', 0), ('unknown', 0), ('incompatible', errno.ENOTSUP),
             ('conflict', errno.ENOTSUP), ('efi', errno.EBADMSG if args.syslinux_uefi else errno.ENOTSUP)]
    if args.syslinux_uefi:
        cases.append(('bios', errno.ENOTSUP))
    for name, expected in cases:
        operation = 'syslinux-ntfs-uefi' if args.syslinux_uefi else 'syslinux-ntfs'
        checked = subprocess.run([args.build / 'iso_write_check_probe', base / (name + '.iso'),
                                  str(HANDOFF_DISK_BYTES), '4096', 'RUFUS_EXT', operation],
                                 capture_output=True, text=True, timeout=20)
        report = json.loads(checked.stdout)
        assert checked.returncode == (0 if expected == 0 else 1) and report['error'] == expected, (name, report, checked.stderr)
        reports[name] = report
    (base / 'source-preflight.json').write_text(json.dumps(reports, indent=2) + '\n')



def check_fat_volume(base, volume, standard=False, log="fsck.log", label="RUFUS TEST"):
    checked = subprocess.run(["fsck.fat", "-n", str(volume)], capture_output=True, timeout=20)
    (base / log).write_bytes(checked.stdout + checked.stderr)
    if not standard:
        assert checked.returncode == 0, checked.stdout + checked.stderr
        return
    # Original ms-sys records intentionally leave NO NAME in the BPB while
    # preserving the directory label. Accept only that precise diagnostic.
    assert checked.returncode == 1 and not checked.stderr, checked
    lines = checked.stdout.decode().splitlines()
    assert lines[1:5] == [f"There is no label in boot sector, but there is volume label '{label}' stored in root directory",
        "  Auto-copying volume label from root directory to boot sector.", "", "Leaving filesystem unchanged."], lines
    assert len(lines) == 6 and re.fullmatch(re.escape(str(volume)) + r": \d+ files, \d+/\d+ clusters", lines[5]), lines


def inspect_fat16_volume(base, disk, offset, size, standard=False, volume_id=0x1234abcd, label="RUFUS TEST"):
    # FAT16 has no backup VBR. The adapter checks the original BPB across
    # installation; independently check its geometry and final filesystem.
    with disk.open("rb") as stream:
        stream.seek(offset)
        boot_sector = stream.read(512)
    sector, spc, reserved, fats, entries, short_total = struct.unpack_from("<HBHBHH", boot_sector, 11)
    fat_size = struct.unpack_from("<H", boot_sector, 22)[0]
    total = short_total or struct.unpack_from("<I", boot_sector, 32)[0]
    assert sector == SECTOR_BYTES and spc * sector == FAT16_CLUSTER_BYTES and fats == 2 and entries >= 512
    assert boot_sector[54:62] == b"FAT16   " and boot_sector[-2:] == b"\x55\xaa"
    assert struct.unpack_from("<I", boot_sector, 28)[0] == offset // sector
    assert struct.unpack_from("<I", boot_sector, 39)[0] == volume_id
    assert boot_sector[43:54] == (b"NO NAME    " if standard else label.encode().ljust(11))
    clusters = (total - reserved - fats * fat_size - entries * 32 // sector) // spc
    assert 4085 <= clusters <= 65524 and 0 <= size - total * sector < 63 * sector
    volume = base / "volume.raw"
    copy_volume(disk, volume, offset, size)
    check_fat_volume(base, volume, standard=standard, label=label)


def inspect_syslinux_ntfs(args, base, disk):
    from syslinux_ntfs import readback
    payload = args.boot_payload.stat().st_size if args.syslinux_uefi else 0
    offset, size = extent(HANDOFF_DISK_BYTES, 4096, uefi_payload=payload)
    with disk.open("rb") as stream:
        prefix = stream.read(offset)
        signature = struct.unpack_from("<I", prefix, 440)[0]
        assert prefix[512:] == bytes(offset - 512), "Changed alignment gap"
        stream.seek(offset + size - SECTOR_BYTES)
        backup = stream.read(SECTOR_BYTES)
        tail = stream.read()
    if payload:
        from test_mbr_image import trailing_extent
        boot_offset, boot_size = trailing_extent(HANDOFF_DISK_BYTES, payload)
        with disk.open("rb") as stream:
            stream.seek(boot_offset)
            assert stream.read(payload) == args.boot_payload.read_bytes(), "UEFI:NTFS payload differs"
        padding = boot_offset - offset - size
        # The seed fills unused space with S. Rufus clears bounded metadata
        # ranges, leaving this gap before the boot partition untouched.
        assert tail[:padding] == b"S" * padding, "Changed gap before UEFI:NTFS"
        assert tail[padding + payload:] == bytes(len(tail) - padding - payload), "Changed trailing padding"
    else:
        assert tail == bytes(len(tail)), "Changed trailing gap"
    inspect_mbr(disk, HANDOFF_DISK_BYTES, 4096, signature=signature, file_system="ntfs",
                uefi_payload=payload, boot_code="syslinux")
    expected = {"layout": {"OFFSET": offset, "SIZE": size}, "prefix": prefix, "boot": backup, "tail": tail}
    # Shared independent file-record/BPB/backup/boot checks from CP20k.
    args.adapter = True
    result = readback(args, base, args.syslinux_ntfs, expected, 0, disk, payload)
    if payload:
        boot_syslinux_uefi(args, base / "uefi-boot", disk)
        result.update({"uefi_boot": True, "boot_offset": boot_offset, "boot_size": boot_size,
                       "uefi_payload_sha256": digest(args.boot_payload)})
    return result


def boot_syslinux_uefi(args, output, disk, native_fat=False):
    from boot_linux import firmware_options
    output.mkdir()
    original = digest(disk)
    command = ['qemu-system-x86_64', '-accel', args.accel, '-m', '256', '-nic', 'none',
               '-display', 'none', '-monitor', 'none', '-no-reboot', '-snapshot',
               '-serial', 'file:' + str(output / 'serial.log'),
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04', '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,bootindex=1']
    command.extend(firmware_options(output, 'uefi', 'syslinux'))
    (output / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    try:
        with (output / 'qemu.log').open('wb') as log:
            boot = subprocess.run(command, timeout=60, env=dict(os.environ, TMPDIR=str(output)),
                                  stdout=log, stderr=subprocess.STDOUT)
        serial = (output / 'serial.log').read_text(errors='replace')
        assert boot.returncode == 33 and 'RUFUS_SYSLINUX_UEFI_OK' in serial, (boot.returncode, serial)
        if args.runtime_validation:
            assert '5/5 files processed [0 failed]' in serial and 'Continue with boot?' not in serial, serial
        if native_fat:
            assert 'UEFI:NTFS' not in serial, "Native FAT boot unexpectedly used UEFI:NTFS"
        else:
            assert 'UEFI:NTFS' in serial, "Original UEFI:NTFS handoff banner missing"
        assert digest(disk) == original, "Firmware boot changed its backing disk"
    finally:
        for firmware in output.glob('*.fd'):
            firmware.unlink()


def inspect_install(args, base, disk):
    from verify import boot, check_files, check_volume
    signature = 0x78563412
    if args.helper or args.syslinux_major:
        with disk.open("rb") as stream:
            stream.seek(440)
            signature = struct.unpack("<I", stream.read(4))[0]
    cluster = FAT16_CLUSTER_BYTES if args.fat_bits == 16 else 512
    inspect_mbr(disk, HANDOFF_DISK_BYTES, cluster=cluster, signature=signature, file_system=args.fat_bits)
    offset, size = extent(HANDOFF_DISK_BYTES, cluster)
    files = {}
    for path in sorted((base / "expected").rglob("*")):
        if path.is_file():
            files['/' + path.relative_to(base / "expected").as_posix()] = path
    if args.syslinux_major != 4:
        files['/ldlinux.c32'] = args.upstream / "bios/com32/elflink/ldlinux/ldlinux.c32"
    env = dict(os.environ, LC_ALL="C.UTF-8" if args.zip_supplement else "C", MTOOLSRC="/dev/null", TMPDIR=str(base))
    check_files(f"{disk}@@{offset}", base, env, files)
    with disk.open("rb") as stream:
        original_boot = None
        if args.fat_bits == 32:
            stream.seek(offset + 6 * SECTOR_BYTES)
            original_boot = stream.read(SECTOR_BYTES)
        stream.seek(512)
        expected_gap = bytearray(offset - 512)
        if args.ffu_partial:
            expected_gap[2 * 131072 - 512:3 * 131072 - 512] = bytes([0x5c]) * 131072
        assert stream.read(offset - 512) == expected_gap, "Installer/partial update changed the unexpected alignment gap"
    if args.fat_bits == 16:
        inspect_fat16_volume(base, disk, offset, size)
    else:
        check_volume(disk, base, offset, size, original_boot, env)
    if args.syslinux_major:
        from test_standalone import boot_prompt
        spec = f"{disk}@@{offset}"
        listing = run(["mdir", "-a", "-b", "-/", "-i", spec, "::/"], capture_output=True, text=True).stdout
        names = {"ldlinux.sys"} if args.syslinux_major == 4 else {"ldlinux.sys", "ldlinux.c32"}
        assert {name.removeprefix("::/").lower() for name in listing.splitlines()} == names
        loader = run(["mtype", "-i", spec, "::/ldlinux.sys"], capture_output=True).stdout
        assert len(loader) == (37888 if args.syslinux_major == 4 else 69632)
        attributes = run(["mattrib", "-i", spec] + [f"::/{name}" for name in sorted(names)], capture_output=True, text=True).stdout
        assert len(attributes.splitlines()) == len(names)
        for line in attributes.splitlines():
            assert {"R", "H", "S"} <= set(line.split("::/", 1)[0]), line
        output = base / "bios"
        output.mkdir()
        version = "4.07" if args.syslinux_major == 4 else "6.04"
        boot_prompt(disk, output, env, version)
        for binary in output.glob("*.bin"):
            binary.unlink()
        boot_result = f"bare Syslinux {version} prompt"
    else:
        boot(disk, env)
        boot_result = "RUFUS_RAW_BOOT_OK"
    result = {"files": {name: digest(path) for name, path in files.items()}, "disk_sha256": digest(disk), "boot": boot_result}
    if args.compressed_raw_helper:
        serial = (base / "serial-512.log").read_text()
        decoded_hash = re.search(r"RUFUS_COMPRESSED_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
        assert decoded_hash and decoded_hash[1] == result["disk_sha256"], result
        result["gzip_decoded_sha256"] = decoded_hash[1]
        if args.xz_raw_helper:
            xz_hash = re.search(r"RUFUS_XZ_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
            assert xz_hash and xz_hash[1] == result["disk_sha256"], result
            result["xz_decoded_sha256"] = xz_hash[1]
        if args.bzip2_raw_helper:
            bzip2_hash = re.search(r"RUFUS_BZIP2_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
            assert bzip2_hash and bzip2_hash[1] == result["disk_sha256"], result
            result["bzip2_decoded_sha256"] = bzip2_hash[1]
    if args.bzip2_qt_helper:
        serial = (base / "serial-512.log").read_text()
        bzip2_hash = re.search(r"RUFUS_BZIP2_QT_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
        assert bzip2_hash and bzip2_hash[1] == result["disk_sha256"], result
        result["bzip2_decoded_sha256"] = bzip2_hash[1]
    if args.lzma_raw_helper:
        serial = (base / "serial-512.log").read_text()
        lzma_hash = re.search(r"RUFUS_LZMA_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
        assert lzma_hash and lzma_hash[1] == result["disk_sha256"], result
        result["lzma_decoded_sha256"] = lzma_hash[1]
    if args.lzw_raw_helper:
        serial = (base / "serial-512.log").read_text()
        lzw_hash = re.search(r"RUFUS_LZW_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
        assert lzw_hash and lzw_hash[1] == result["disk_sha256"], result
        result["lzw_decoded_sha256"] = lzw_hash[1]
    if args.zip_raw_helper:
        serial = (base / "serial-512.log").read_text()
        zip_hash = re.search(r"RUFUS_ZIP_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
        assert zip_hash and zip_hash[1] == result["disk_sha256"], result
        result["zip_decoded_sha256"] = zip_hash[1]
    if args.zstd_raw_helper:
        serial = (base / "serial-512.log").read_text()
        zstd_hash = re.search(r"RUFUS_ZSTD_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
        assert zstd_hash and zstd_hash[1] == result["disk_sha256"], result
        result["zstd_decoded_sha256"] = zstd_hash[1]
    if args.vtsi_raw_helper:
        serial = (base / "serial-512.log").read_text()
        vtsi_hash = re.search(r"RUFUS_VTSI_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
        assert vtsi_hash and vtsi_hash[1] == result["disk_sha256"], result
        result["vtsi_decoded_sha256"] = vtsi_hash[1]
    if args.vhd_raw_helper:
        serial = (base / "serial-512.log").read_text()
        vhd_hash = re.search(r"RUFUS_VHD_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
        assert vhd_hash and vhd_hash[1] == result["disk_sha256"], result
        result["vhd_decoded_sha256"] = vhd_hash[1]
    if args.ffu_qt_helper:
        serial = (base / "serial-512.log").read_text()
        marker = r"RUFUS_FFU_OPTIMIZED_DONE SHA256=([0-9a-f]{64})" if args.ffu_optimized else r"RUFUS_FFU_STORES_DONE SHA256=([0-9a-f]{64})" if args.ffu_stores else r"RUFUS_OLD_BIOS_DONE SHA256=([0-9a-f]{64})" if args.old_bios else r"RUFUS_APPEARANCE_DONE SHA256=([0-9a-f]{64})" if args.media_appearance else r"RUFUS_BADBLOCK_DONE SHA256=([0-9a-f]{64})" if args.bad_blocks else r"RUFUS_FULL_FORMAT_DONE SHA256=([0-9a-f]{64})" if args.full_format else r"RUFUS_ZERO_DEVICE_DONE SHA256=([0-9a-f]{64})" if args.zero_device else r"RUFUS_IMAGE_ENTRY_DONE SHA256=([0-9a-f]{64})" if args.image_entry else r"RUFUS_FFU_PARTIAL_DONE SHA256=([0-9a-f]{64})" if args.ffu_partial else r"RUFUS_FFU_GEOMETRY_DONE SHA256=([0-9a-f]{64})" if args.ffu_geometry else r"RUFUS_FFU_VARIANTS_DONE SHA256=([0-9a-f]{64})" if args.ffu_variants else r"RUFUS_FFU_QT_HELPER_DONE SHA256=([0-9a-f]{64})"
        ffu_hash = re.search(marker, serial)
        assert ffu_hash and ffu_hash[1] == result["disk_sha256"], result
        result["ffu_decoded_sha256"] = ffu_hash[1]
    if args.old_bios:
        result['old_bios'] = inspect_old_bios_boot(args, base)
    if args.vhdx_qt_helper:
        serial = (base / "serial-512.log").read_text()
        marker = r"RUFUS_VIRTUAL_DISK_DONE SHA256=([0-9a-f]{64})" if args.virtual_disk_chains else r"RUFUS_VHDX_QT_HELPER_DONE SHA256=([0-9a-f]{64})"
        vhdx_hash = re.search(marker, serial)
        assert vhdx_hash and vhdx_hash[1] == result["disk_sha256"], result
        result["vhdx_decoded_sha256"] = vhdx_hash[1]
    if args.lzma_qt_helper:
        serial = (base / "serial-512.log").read_text()
        lzma_hash = re.search(r"RUFUS_LZMA_QT_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
        assert lzma_hash and lzma_hash[1] == result["disk_sha256"], result
        result["lzma_decoded_sha256"] = lzma_hash[1]
    if args.lzw_qt_helper:
        serial = (base / "serial-512.log").read_text()
        lzw_hash = re.search(r"RUFUS_LZW_QT_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
        assert lzw_hash and lzw_hash[1] == result["disk_sha256"], result
        result["lzw_decoded_sha256"] = lzw_hash[1]
    if args.zip_qt_helper:
        serial = (base / "serial-512.log").read_text()
        zip_hash = re.search(r"RUFUS_ZIP_QT_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
        assert zip_hash and zip_hash[1] == result["disk_sha256"], result
        result["zip_decoded_sha256"] = zip_hash[1]
    if args.zstd_qt_helper:
        serial = (base / "serial-512.log").read_text()
        zstd_hash = re.search(r"RUFUS_ZSTD_QT_HELPER_DONE SHA256=([0-9a-f]{64})", serial)
        assert zstd_hash and zstd_hash[1] == result["disk_sha256"], result
        result["zstd_decoded_sha256"] = zstd_hash[1]
    return result


def inspect_exfat(args, base, disk):
    from test_partition_image import inspect_gpt, extent as gpt_extent
    from test_exfat_format import inspect
    args.sfdisk = "/usr/bin/sfdisk"
    args.fsck = "/usr/bin/fsck.exfat"
    args.dump = "/usr/bin/dump.exfat"
    cluster = 128 * 1024 if args.exfat_helper else 4096
    if args.sfd_exfat_helper:
        offset, size = 0, HANDOFF_DISK_BYTES
        table = json.loads(run([args.sfdisk, "--json", disk], capture_output=True).stdout)["partitiontable"]
        assert table["label"] == "dos" and not table.get("partitions"), table
        assert table.get("id", "0x00000000") == "0x00000000", table
    elif args.mbr_exfat_helper:
        if not args.standalone_uefi:
            inspect_mbr(disk, HANDOFF_DISK_BYTES, cluster, file_system="exfat", non_bootable=True)
        offset, size = extent(HANDOFF_DISK_BYTES, cluster)
    else:
        table_cluster = cluster if args.exfat_helper else SECTOR_BYTES
        if not args.standalone_uefi:
            inspect_gpt(args, disk, HANDOFF_DISK_BYTES, table_cluster)
        offset, size = gpt_extent(HANDOFF_DISK_BYTES, table_cluster)
    text = (base / "serial-512.log").read_text(errors="replace")
    marker = "RUFUS_FORMAT_VOLUME" if args.exfat_helper else "RUFUS_EXFAT_VOLUME"
    reports = re.findall(marker + r" ([^\r\n]+)", text)
    assert len(reports) == (2 if args.exfat_helper else 1)
    for entry in reports:
        report = values(entry)
        assert (report["OFFSET"], report["SIZE"]) == (offset, size)
        if args.exfat_helper:
            assert report["BYTES"] == size
    boot = {}
    if args.standalone_uefi:
        offset, size, boot = inspect_uefi_payload(args, disk, cluster)
    volume = base / "volume.raw"
    copy_volume(disk, volume, offset, size)
    inspect(args, volume, size, cluster, "Rufus ș😀", free_tail=False, partition_offset=offset)
    return {"offset": offset, "size": size, "disk_sha256": digest(disk), **boot}


def make_grub4dos_image_fixture(args, base):
    tree = base / "grub4dos-source"
    tree.mkdir()
    shutil.copy2(prepare.HERE.parent / "fixtures/download/grub4dos-0.4.6a/grldr", tree / "grldr")
    (tree / "menu.lst").write_text("timeout 0\ndefault 0\ntitle Rufus ISO test\n"
                                  "set RUFUSISO=RUFUSGRUB4DOSIMAGE\ncommandline\n")
    (tree / "image-marker").write_text("RUFUSGRUB4DOSIMAGE\n")
    (tree / "payload.bin").write_bytes(bytes(range(256)) * 32 + b'!')
    if args.grub4dos_image_uefi:
        efi = tree / "EFI/BOOT/BOOTX64.EFI"
        efi.parent.mkdir(parents=True)
        shutil.copy2(args.build / "uefi-handoff.efi", efi)
    args.iso = base / "grub4dos-source.iso"
    command = [args.mkisofs, "-R", "-J", "-V", "SOURCE", "-o", args.iso, tree]
    with (base / "mkisofs.log").open("wb") as log:
        subprocess.run(command, check=True, timeout=30, stdout=log, stderr=subprocess.STDOUT)
    (base / "image-files.json").write_text(json.dumps({path.relative_to(tree).as_posix(): {"bytes": path.stat().st_size,
        "sha256": digest(path)} for path in sorted(tree.rglob("*")) if path.is_file()}, indent=2) + "\n")


def inspect_grub4dos(args, base, disk):
    from test_freedos_boot import expected_records
    with disk.open("rb") as stream:
        mbr = stream.read(512)
        signature = struct.unpack_from("<I", mbr, 440)[0]
    cluster = 4096 if args.grub4dos_ntfs else FAT16_CLUSTER_BYTES if args.fat_bits == 16 else SECTOR_BYTES
    filesystem = "ntfs" if args.grub4dos_ntfs else args.fat_bits
    paired = args.grub4dos_ntfs and args.grub4dos_image_uefi
    payload_bytes = args.boot_payload.stat().st_size if paired else 0
    inspect_mbr(disk, HANDOFF_DISK_BYTES, cluster, signature=signature,
                file_system=filesystem, boot_code="grub4dos", uefi_payload=payload_bytes)
    offset, size = extent(HANDOFF_DISK_BYTES, cluster, uefi_payload=payload_bytes)
    resource = (prepare.HERE.parents[2] / "res/grub/grldr.mbr").read_bytes()
    with disk.open("rb") as stream:
        stream.seek(512)
        assert stream.read(7680) == resource[512:]
        assert stream.read(offset - 8192) == bytes(offset - 8192)
    if args.grub4dos_ntfs:
        result = inspect_grub4dos_ntfs(args, base, disk, offset, size)
        if paired:
            boot_offset, boot_size = trailing_extent(HANDOFF_DISK_BYTES, payload_bytes)
            with disk.open("rb") as stream:
                stream.seek(boot_offset)
                assert stream.read(payload_bytes) == args.boot_payload.read_bytes(), "Original UEFI:NTFS payload"
            result.update({"boot_offset": boot_offset, "boot_size": boot_size,
                "uefi_payload_sha256": digest(args.boot_payload)})
        return result
    with disk.open("rb") as stream:
        stream.seek(offset)
        boot = stream.read(512)
        stream.seek(offset)
        records = stream.read(19 * SECTOR_BYTES)
        assert records == expected_records(records, args.fat_bits, standard=True), "Original standard FAT boot records"
        short_total = struct.unpack_from("<H", boot, 19)[0] if args.fat_bits == 16 else 0
        total = (short_total or struct.unpack_from("<I", boot, 32)[0]) * 512
        fs_offset = 54 if args.fat_bits == 16 else 82
        assert boot[fs_offset:fs_offset + 8] == f"FAT{args.fat_bits}".encode().ljust(8) and boot[510:] == b"\x55\xaa"
        assert struct.unpack_from("<I", boot, 28)[0] == offset // 512
        serial = (base / "serial-512.log").read_text()
        report_pattern = r"RUFUS_GRUB4DOS_IMAGE_DONE ([^\r\n]+)" if args.grub4dos_image else r"RUFUS_GRUB4DOS_DONE ([^\r\n]+)"
        report = values(re.search(report_pattern, serial).group(1))
        volume_id = report["VOLUMEID"] if args.grub4dos_gui or args.grub4dos_image_gui else 0x1234abcd
        id_offset = 39 if args.fat_bits == 16 else 67
        assert struct.unpack_from("<I", boot, id_offset)[0] == volume_id
        if args.fat_bits == 32:
            stream.seek(offset + 6 * 512)
            assert stream.read(512) == boot
    report = values(re.search(report_pattern,
                             (base / "serial-512.log").read_text()).group(1))
    assert (report["OFFSET"], report["SIZE"], report["BYTES"]) == (offset, size, total)
    label = "RUFUS_TEST" if args.grub4dos_gui or args.grub4dos_image else "RUFUS TEST"
    if args.fat_bits == 16:
        inspect_fat16_volume(base, disk, offset, size, standard=True, volume_id=volume_id, label=label)
    else:
        volume = base / "volume.raw"
        copy_volume(disk, volume, offset, size)
        check_fat_volume(base, volume, standard=True, label=label)
    copied = subprocess.run(["mtype", "-i", f"{disk}@@{offset}", "::/grldr"],
                            capture_output=True, check=True, timeout=10).stdout
    expected = (prepare.HERE.parent / "fixtures/download/grub4dos-0.4.6a/grldr").read_bytes()
    assert copied == expected
    listing_options = ["-b", "-s"] if args.grub4dos_image else ["-b"]
    listing = subprocess.run(["mdir", *listing_options, "-i", f"{disk}@@{offset}", "::/"],
                             capture_output=True, check=True, timeout=10)
    (base / "mdir.log").write_bytes(listing.stdout + listing.stderr)
    if args.grub4dos_image:
        expected_files = json.loads((base / "image-files.json").read_text())
        names = {line.decode().removeprefix('::/') for line in listing.stdout.splitlines() if not line.endswith(b'/')}
        assert names == set(expected_files), names
        for name in expected_files:
            copied_file = subprocess.run(["mtype", "-i", f"{disk}@@{offset}", "::/" + name],
                                         capture_output=True, check=True, timeout=10).stdout
            assert copied_file == (base / "grub4dos-source" / name).read_bytes(), name
        assert report["COPIED"] == sum(entry["bytes"] for entry in expected_files.values()), report
    else:
        assert listing.stdout.splitlines() == [b"::/grldr"]
    return {"offset": offset, "size": size, "filesystem_bytes": total,
            "loader_bytes": len(copied), "disk_sha256": digest(disk), "boot_execution_qualified": False}


def inspect_grub4dos_ntfs(args, base, disk, offset, size):
    from test_ntfs_boot import original_code
    with disk.open("rb") as stream:
        stream.seek(offset)
        boot = stream.read(4136)
        stream.seek(offset + size - SECTOR_BYTES)
        backup = stream.read(SECTOR_BYTES)
    assert boot[:11] == original_code("br_ntfs_0x0.h")
    assert boot[0x54:] == original_code("br_ntfs_0x54.h")
    assert boot[11:0x54] == backup[11:0x54]
    assert boot[:SECTOR_BYTES] != backup
    total = (struct.unpack_from("<Q", boot, 40)[0] + 1) * SECTOR_BYTES
    report_pattern = r"RUFUS_GRUB4DOS_IMAGE_DONE ([^\r\n]+)" if args.grub4dos_image else r"RUFUS_GRUB4DOS_DONE ([^\r\n]+)"
    report = values(re.search(report_pattern,
                             (base / "serial-512.log").read_text()).group(1))
    assert (report["OFFSET"], report["SIZE"], report["BYTES"]) == (offset, size, total)
    volume = base / "volume.raw"
    copy_volume(disk, volume, offset, size)
    label = subprocess.run(["/usr/bin/ntfslabel", volume], capture_output=True, timeout=15, check=True)
    expected_label = "RUFUS_TEST" if args.grub4dos_image else "Rufus ș😀_test" if args.grub4dos_gui else "RUFUS TEST"
    assert label.stdout.decode().strip() == expected_label
    assert struct.unpack_from("<Q", boot, 72)[0] != 0, "Native formatter supplied an NTFS serial"
    if args.grub4dos_gui or args.grub4dos_image_gui:
        assert report["VOLUMEID"] == 0, "Qt leaves NTFS serial ownership to the formatter"
    info = subprocess.run(["/usr/bin/ntfsinfo", "-m", volume], capture_output=True, timeout=15, check=True)
    (base / "ntfsinfo.log").write_bytes(info.stdout + info.stderr)
    assert b"Volume Version: 3.1" in info.stdout
    assert re.search(rb"Cluster Size:\s+4096", info.stdout)
    assert re.search(rb"Volume Flags:\s+0x0000", info.stdout)
    listing = subprocess.run(["7z", "l", "-slt", volume], capture_output=True, timeout=15, check=True)
    (base / "7z-list.log").write_bytes(listing.stdout + listing.stderr)
    copied = subprocess.run(["7z", "x", "-so", volume, "grldr"], capture_output=True, timeout=15, check=True).stdout
    other_reader = subprocess.run([args.build / "ntfs_file_fixture", "get", volume, "grldr"],
                                  capture_output=True, timeout=15, check=True).stdout
    expected = (prepare.HERE.parent / "fixtures/download/grub4dos-0.4.6a/grldr").read_bytes()
    assert copied == other_reader == expected
    entries = re.findall(rb"^Path = (.+)\r?\nFolder = ([-+])\r?$", listing.stdout, re.M)
    user_paths = {path.decode() for path, folder in entries
                  if folder == b"-" and not path.startswith(b"[SYSTEM]/")}
    if args.grub4dos_image:
        expected_files = json.loads((base / "image-files.json").read_text())
        assert user_paths == set(expected_files), user_paths
        for name in expected_files:
            copied_file = subprocess.run(["7z", "x", "-so", volume, name],
                                         capture_output=True, timeout=15, check=True).stdout
            other_file = subprocess.run([args.build / "ntfs_file_fixture", "get", volume, name],
                                        capture_output=True, timeout=15, check=True).stdout
            assert copied_file == other_file == (base / "grub4dos-source" / name).read_bytes(), name
        assert report["COPIED"] == sum(entry["bytes"] for entry in expected_files.values()), report
    else:
        assert user_paths == {"grldr"}, user_paths
    return {"offset": offset, "size": size, "filesystem": "NTFS", "filesystem_bytes": total,
            "loader_bytes": len(copied), "disk_sha256": digest(disk), "boot_execution_qualified": False}


def inspect_fat_format(args, base, disk):
    from test_partition_image import inspect_gpt, extent as gpt_extent
    args.sfdisk = "/usr/bin/sfdisk"
    cluster = FAT16_CLUSTER_BYTES if args.fat_bits == 16 else SECTOR_BYTES
    if args.mbr_fat_helper or args.grub2_helper:
        with disk.open('rb') as stream:
            signature = struct.unpack_from('<I', stream.read(512), 440)[0]
        inspect_mbr(disk, HANDOFF_DISK_BYTES, cluster, file_system=args.fat_bits,
                    non_bootable=not args.grub2_helper, signature=signature,
                    boot_code='grub2' if args.grub2_helper else None)
        offset, size = extent(HANDOFF_DISK_BYTES, cluster)
    else:
        inspect_gpt(args, disk, HANDOFF_DISK_BYTES, cluster)
        offset, size = gpt_extent(HANDOFF_DISK_BYTES, cluster)
    text = (base / "serial-512.log").read_text(errors="replace")
    reports = re.findall(r"RUFUS_FORMAT_VOLUME ([^\r\n]+)", text)
    assert len(reports) == 2  # Interrupted sync retry and ordinary success.
    for report in reports:
        result = values(report)
        assert (result["OFFSET"], result["SIZE"]) == (offset, size)
    with disk.open("rb") as stream:
        stream.seek(offset)
        boot = stream.read(SECTOR_BYTES)
        short_total = struct.unpack_from("<H", boot, 19)[0]
        total = short_total or struct.unpack_from("<I", boot, 32)[0]
        assert total * SECTOR_BYTES == result["BYTES"]
        if args.fat_bits == 32:
            sector, spc, reserved, fats = struct.unpack_from("<HBHB", boot, 11)
            fat_size, root_cluster = (struct.unpack_from("<I", boot, pos)[0] for pos in (36, 44))
            assert sector == SECTOR_BYTES and spc * sector == cluster and fats == 2
            assert boot[510:] == b"\x55\xaa" and boot[82:90] == b"FAT32   "
            assert struct.unpack_from("<I", boot, 28)[0] == offset // sector
            assert struct.unpack_from("<I", boot, 67)[0] == 0x1234abcd
            assert boot[71:82] == (b"NO NAME    " if args.grub2_helper else b"RUFUS TEST ")
            assert 0 <= size - total * sector < 63 * sector
            clusters = (total - reserved - fats * fat_size) // spc
            assert 65525 <= clusters <= 0x0ffffff4 and root_cluster == 2
            stream.seek(offset + 6 * sector)
            assert stream.read(SECTOR_BYTES) == boot
    if args.fat_bits == 16:
        inspect_fat16_volume(base, disk, offset, size, standard=args.grub2_helper)
    else:
        volume = base / "volume.raw"
        copy_volume(disk, volume, offset, size)
        check_fat_volume(base, volume, standard=args.grub2_helper)
    listing = subprocess.run(["mdir", "-i", f"{disk}@@{offset}", "::"], capture_output=True, timeout=10)
    (base / "mdir.log").write_bytes(listing.stdout + listing.stderr)
    assert listing.returncode == 0 and b"No files" in listing.stdout, listing.stdout + listing.stderr
    return {"offset": offset, "size": size, "filesystem_bytes": result["BYTES"], "disk_sha256": digest(disk)}


def inspect_old_bios_boot(args, base):
    import hashlib
    import importlib.util
    path = prepare.HERE.parents[1] / 'probes/grub2/verify.py'
    spec = importlib.util.spec_from_file_location('rufus_old_bios_grub_verify', path)
    grub = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(grub)
    disk = base / 'old-bios-boot.raw'
    serial = (base / 'serial-512.log').read_text()
    saved = re.search(r'RUFUS_OLD_BIOS_BOOT_SNAPSHOT BYTES=(\d+) SHA256=([0-9a-f]{64})', serial)
    assert saved and int(saved[1]) == HANDOFF_DISK_BYTES
    with disk.open('rb') as stream:
        raw = stream.read(HANDOFF_DISK_BYTES)
    assert hashlib.sha256(raw).hexdigest() == saved[2]
    mbr = raw[:512]
    offset = struct.unpack_from('<I', mbr, 454)[0] * 512
    size = struct.unpack_from('<I', mbr, 458)[0] * 512
    assert offset == 64512 and mbr[446] == 0x80 and mbr[466] == 0xea
    _, core, code = grub.check_payloads()
    assert raw[:len(code)] == code and raw[512:512 + len(core)] == core
    assert raw[512 + len(core):offset] == bytes(offset - 512 - len(core))
    volume = base / 'old-bios-fat.raw'
    copy_volume(disk, volume, offset, size)
    check_fat_volume(base, volume, standard=True, log='old-bios-fsck.log', label='OLD BIOS')
    volume.unlink()
    boot = base / 'old-bios-bios'; boot.mkdir()
    result = grub.boot_disk(argparse.Namespace(qemu='qemu-system-x86_64'), disk, boot, boot, marker=None)
    return {'offset': offset, 'size': size, 'snapshot_sha256': saved[2], 'bios_boot': result}


def inspect_grub2(args, base, disk):
    from test_freedos_boot import read_window, expected_records, check_directory
    sys.path.insert(0, str(prepare.HERE.parents[1] / 'probes/grub2'))
    from verify import check_payloads, boot_disk
    result = inspect_fat_format(args, base, disk)
    core_path, core, code = check_payloads()
    with disk.open('rb') as stream:
        prefix = stream.read(result['offset'])
    assert prefix[:len(code)] == code
    assert prefix[432:440] == bytes(8)
    assert prefix[512:512 + len(core)] == core
    assert prefix[512 + len(core):] == bytes(len(prefix) - 512 - len(core))
    window = read_window(disk, result['offset'])
    assert window == expected_records(window, args.fat_bits, standard=True)
    check_directory(argparse.Namespace(mdir='/usr/bin/mdir'), disk, args.fat_bits, result['offset'], 'RUFUS TEST')
    boot = base / 'grub2-bios'
    boot.mkdir()
    evidence = boot_disk(argparse.Namespace(qemu='qemu-system-x86_64'), disk, boot, boot, marker=None)
    return {**result, 'core_sha256': digest(core_path), 'standard_pbr': True, 'bios_boot': evidence}


def inspect_grub2_ntfs(args, base, disk):
    from test_ntfs_boot import original_code
    sys.path.insert(0, str(prepare.HERE.parents[1] / 'probes/grub2'))
    from verify import check_payloads, boot_disk
    cluster = 4096
    with disk.open('rb') as stream:
        signature = struct.unpack_from('<I', stream.read(SECTOR_BYTES), 440)[0]
    inspect_mbr(disk, HANDOFF_DISK_BYTES, cluster, file_system="ntfs",
                boot_code="grub2", signature=signature)
    offset, size = extent(HANDOFF_DISK_BYTES, cluster)
    core_path, core, code = check_payloads()
    with disk.open('rb') as stream:
        prefix = stream.read(offset)
        boot = stream.read(4136)
        stream.seek(offset + size - SECTOR_BYTES)
        backup = stream.read(SECTOR_BYTES)
    assert prefix[:len(code)] == code
    assert prefix[512:512 + len(core)] == core
    assert prefix[512 + len(core):] == bytes(len(prefix) - 512 - len(core))
    assert boot[:11] == original_code('br_ntfs_0x0.h')
    assert boot[0x54:] == original_code('br_ntfs_0x54.h')
    assert boot[11:0x54] == backup[11:0x54]
    assert boot[:SECTOR_BYTES] != backup
    volume = base / 'volume.raw'
    copy_volume(disk, volume, offset, size)
    label = subprocess.run(['/usr/bin/ntfslabel', volume], capture_output=True, timeout=15, check=True)
    assert label.stdout.decode().strip() == 'Rufus ș😀'
    info = subprocess.run(['/usr/bin/ntfsinfo', '-m', volume], capture_output=True, timeout=15, check=True)
    assert b'Volume Version: 3.1' in info.stdout
    bios = base / 'grub2-ntfs-bios'
    bios.mkdir()
    evidence = boot_disk(argparse.Namespace(qemu='qemu-system-x86_64'), disk, bios, bios,
                         marker=None, filesystem='ntfs')
    return {'offset': offset, 'size': size, 'disk_sha256': digest(disk),
            'core_sha256': digest(core_path), 'bios_boot': evidence}


def inspect_grub2_ext(args, base, disk):
    from test_ext_format import inspect as inspect_ext
    sys.path.insert(0, str(prepare.HERE.parents[1] / 'probes/grub2'))
    from verify import check_payloads, boot_disk
    file_system = f'ext{args.ext_bits}'
    cluster = 0x100
    with disk.open('rb') as stream:
        signature = struct.unpack_from('<I', stream.read(SECTOR_BYTES), 440)[0]
    inspect_mbr(disk, EXT_DISK_BYTES, cluster, file_system=file_system,
                boot_code='grub2', signature=signature)
    offset, size = extent(EXT_DISK_BYTES, cluster)
    core_path, core, code = check_payloads()
    with disk.open('rb') as stream:
        prefix = stream.read(offset)
    assert prefix[:len(code)] == code
    assert prefix[SECTOR_BYTES:SECTOR_BYTES + len(core)] == core
    assert prefix[SECTOR_BYTES + len(core):] == bytes(offset - SECTOR_BYTES - len(core))
    volume = base / 'volume.raw'
    copy_volume(disk, volume, offset, size)
    inspect_ext(argparse.Namespace(dump='/usr/bin/dumpe2fs', fsck='/usr/bin/e2fsck'),
                volume, file_system, size, 'RufusExt', 1024, 128)
    volume.unlink()
    bios = base / 'grub2-ext-bios'
    bios.mkdir()
    evidence = boot_disk(argparse.Namespace(qemu='qemu-system-x86_64'), disk, bios, bios,
                         marker=None, filesystem='ext2')
    return {'offset': offset, 'size': size, 'disk_sha256': digest(disk),
            'core_sha256': digest(core_path), 'bios_boot': evidence}


def inspect_uefi_payload(args, disk, cluster):
    from test_partition_image import inspect_gpt, extent as gpt_extent, trailing_extent
    payload = args.boot_payload.read_bytes()
    if args.uefi_mbr:
        from test_mbr_image import trailing_extent as mbr_tail
        filesystem = "ntfs" if args.ntfs_helper else "exfat"
        inspect_mbr(disk, HANDOFF_DISK_BYTES, cluster, file_system=filesystem, uefi_payload=len(payload))
        offset, size = extent(HANDOFF_DISK_BYTES, cluster, uefi_payload=len(payload))
        boot_offset, boot_size = mbr_tail(HANDOFF_DISK_BYTES, len(payload))
    else:
        inspect_gpt(args, disk, HANDOFF_DISK_BYTES, cluster, payload=len(payload))
        offset, size = gpt_extent(HANDOFF_DISK_BYTES, cluster, payload=len(payload))
        boot_offset, boot_size = trailing_extent(HANDOFF_DISK_BYTES, len(payload))
    with disk.open('rb') as source:
        source.seek(boot_offset)
        assert source.read(len(payload)) == payload, 'Embedded UEFI:NTFS payload changed'
    boot = {'standalone_uefi_ntfs': True, 'boot_offset': boot_offset, 'boot_size': boot_size,
            'payload_sha256': digest(args.boot_payload)}
    return offset, size, boot


def inspect_ntfs_format(args, base, disk):
    from test_partition_image import inspect_gpt, extent as gpt_extent
    from test_ntfs_format import inspect
    args.sfdisk = "/usr/bin/sfdisk"
    args.info = "/usr/bin/ntfsinfo"
    args.label = "/usr/bin/ntfslabel"
    args.fsck = "/usr/bin/ntfsck"
    cluster = 4096
    # The two ordinary format successes precede the optional standalone job.
    if args.sfd_ntfs_helper:
        offset, size = 0, HANDOFF_DISK_BYTES
        table = run([args.sfdisk, "--json", disk], capture_output=True)
        layout = json.loads(table.stdout)["partitiontable"]
        assert layout["label"] == "dos" and not layout.get("partitions"), layout
        assert layout.get("id", "0x00000000") == "0x00000000", layout
    elif args.mbr_ntfs_helper:
        if not args.standalone_uefi:
            inspect_mbr(disk, HANDOFF_DISK_BYTES, cluster, file_system="ntfs", non_bootable=True)
        offset, size = extent(HANDOFF_DISK_BYTES, cluster)
    elif not args.uefi_ntfs_helper:
        inspect_gpt(args, disk, HANDOFF_DISK_BYTES, cluster)
        offset, size = gpt_extent(HANDOFF_DISK_BYTES, cluster)
    else:
        offset, size = gpt_extent(HANDOFF_DISK_BYTES, cluster)
    text = (base / "serial-512.log").read_text(errors="replace")
    reports = re.findall(r"RUFUS_FORMAT_VOLUME ([^\r\n]+)", text)
    assert len(reports) == 2
    for report in reports:
        result = values(report)
        assert (result["OFFSET"], result["SIZE"], result["BYTES"]) == (offset, size, size)
    boot = {}
    if args.standalone_uefi:
        offset, size, boot = inspect_uefi_payload(args, disk, cluster)
    volume = base / "volume.raw"
    copy_volume(disk, volume, offset, size)
    with volume.open("rb") as stream:
        stream.seek(72)
        volume_id = struct.unpack("<Q", stream.read(8))[0]
    inspect(args, volume, size, offset, cluster, "Rufus ș😀", {"VOLUME_ID": volume_id})
    return {"offset": offset, "size": size, "volume_id": volume_id,
            "ntfs_version": "3.1", "clean": True, "disk_sha256": digest(disk), **boot}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--accel", choices=["kvm", "tcg"], default="kvm")
    parser.add_argument("--uefi-mbr", action="store_true", help="Use MBR for a standalone UEFI:NTFS helper qualification")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--uefi-exfat-helper", action="store_true", help="Qualify standalone GPT/exFAT preparation with the embedded UEFI:NTFS payload")
    mode.add_argument("--uefi-ntfs-helper", action="store_true", help="Qualify standalone GPT/NTFS preparation and finalization with the embedded UEFI:NTFS payload")
    mode.add_argument("--ntfs-mounted-helper", action="store_true", help="Qualify complete helper jobs starting from NTFS-3G desktop mounts")
    mode.add_argument("--fixed-usb-helper", action="store_true", help="Qualify explicit fixed-USB native protection and single-launch helper jobs")
    mode.add_argument("--compressed-qt-helper", action="store_true", help="Qualify ordinary gzip SELECT/START/controller through the production guest helper and BIOS boot")
    mode.add_argument("--bzip2-qt-helper", action="store_true", help="Qualify ordinary BZIP2 SELECT/controller through the production helper, required seed/raw setup and full readback/BIOS boot")
    mode.add_argument("--lzma-qt-helper", action="store_true", help="Qualify ordinary LZMA SELECT/controller through one production helper, required seed/raw setup and full readback/BIOS boot")
    mode.add_argument("--bzip2-raw-helper", action="store_true", help="Qualify BZIP2 guest claims/checks/restore alongside all 47 gzip/XZ helper/controller jobs and BIOS boot")
    mode.add_argument("--lzma-raw-helper", action="store_true", help="Qualify focused LZMA real-kernel claims/source/size/history/fault/restore/readback/BIOS jobs with required seed/raw setup")
    mode.add_argument("--lzw-raw-helper", action="store_true", help="Qualify LZW real-kernel format/source/claim/data faults and full poisoned restore/readback/BIOS with required seed/raw setup")
    mode.add_argument("--zip-raw-helper", action="store_true", help="Qualify all five ZIP entry methods on a real guest block device, claims/faults and a full poisoned deflate restore/readback/BIOS boot")
    mode.add_argument("--zstd-raw-helper", action="store_true", help="Qualify Zstd frames, real guest claim/fault jobs and a full poisoned restore/readback/BIOS boot")
    mode.add_argument("--vtsi-raw-helper", action="store_true", help="Qualify VTSI sparse gaps, real guest claim/readback and bootable full restore")
    mode.add_argument("--vhd-raw-helper", action="store_true", help="Qualify fixed/dynamic VHD on a real guest disk, claim/readback and bootable full restore")
    mode.add_argument("--vhd-qt-helper", action="store_true", help="Qualify ordinary VHD SELECT/confirmation/joined READY and full bootable restore")
    parser.add_argument("--save-export", action="store_true", help="Add ISO export and automatic source-unmount cases to the save batch")
    parser.add_argument("--save-read-only", action="store_true", help="Capture read-only devices in all four save formats")
    mode.add_argument("--save-qt-helper", action="store_true", help="Capture VHD/VHDX/FFU together as an unprivileged caller, then restore and boot once")
    parser.add_argument("--zero-device", action="store_true", help="Combine full/fast source-free device clearing, empty fast job and ordinary restore/boot")
    parser.add_argument("--image-entry", action="store_true", help="Combine explicit raw/compressed marker override, default refusal and representative boot")
    parser.add_argument("--ffu-partial", action="store_true", help="Combine conditional partial FFU update, wrong-base refusal, Qt and representative boot")
    parser.add_argument("--media-appearance", action="store_true", help="Combine Unicode labels, icon files, filesystem mounts and one restore")
    parser.add_argument("--bad-blocks", action="store_true", help="Combine device patterns, fake-capacity decisions and one ordinary restore")
    parser.add_argument("--full-format", action="store_true", help="Combine all native full formatters and one bootable restore")
    parser.add_argument("--zip-supplement", action="store_true", help="Combine ZIP prewrite refusals, ordinary Qt formatting and bootable ISO overlay/readback")
    parser.add_argument("--device-source", action="store_true", help="Grouped B2 device and source qualification")
    mode.add_argument("--device-options", action="store_true", help="Grouped native virtual/VMware/removable device qualification")
    mode.add_argument("--iso-resources", action="store_true", help="Combined B3 EFI/extraction/Syslinux FAT and NTFS guest")
    mode.add_argument("--legacy-iso", action="store_true", help="Combined B4 Windows/WinPE FAT16, ReactOS ISO and Kolibri ISO guest")
    mode.add_argument("--geometry-batch", action="store_true", help="Combined B5 512/512e/4Kn, all seven formatters and MBR/GPT/SFD native Qt guest")
    mode.add_argument("--ntfs-options", action="store_true", help="NTFS compression/indexing ordinary Qt and native metadata/boot batch")
    mode.add_argument("--iso-options", action="store_true", help="ISO extension/timestamp options and explicit DOS size override batch")
    mode.add_argument("--runtime-validation", action="store_true", help="Runtime EFI validators, checksum repair, ordinary Qt FAT16/FAT32/NTFS and native EFI handoff")
    mode.add_argument("--legacy-boot",action="store_true",help="Combined MS-DOS/ReactOS FAT16/FAT32 Qt creation, independent readback and bare BIOS handoff")
    parser.add_argument("--msdos-resource",type=Path,help="Original pinned diskcopy.dll, read only as data")
    mode.add_argument("--native-formats", action="store_true", help="Combine ordinary UDF/ext data jobs and independent readback without booting Windows")
    mode.add_argument("--udf-indexing-batch", action="store_true", help="Two ordinary UDF jobs with saved indexing choices, optional diagnostic and independent metadata/payload readback")
    parser.add_argument("--mkudffs", type=Path, help="Explicit UDF provider matching the test build")
    mode.add_argument("--windows-togo", action="store_true", help="Combine WIM/ESD and ISO To Go, GPT/MBR, ordinary Qt and native readback")
    parser.add_argument("--ffu-geometry", action="store_true", help="Combine exact-sector FFU block capture/Qt restore with one full bootable restore")
    parser.add_argument("--old-bios", action="store_true", help="Combine legacy alignment/compatibility helpers, Qt, filesystem readback and BIOS boot")
    parser.add_argument("--ffu-optimized", type=Path, help="Independent optimized FFU fixtures: smaller/larger target, NTFS/recovery updates and one bootable restore")
    parser.add_argument("--ffu-stores", action="store_true", help="Deploy each selected V2 store and perform one full bootable restore")
    parser.add_argument("--ffu-catalog", type=Path, help="Inject a disposable signed catalog into the FFU store batch")
    parser.add_argument("--ffu-layouts", action="store_true", help="Capture an extended GPT with a multi-chunk FFU manifest, then restore and boot once")
    parser.add_argument("--ffu-variants", action="store_true", help="Combine LZNT1/Huffman FFU grouped-descriptor Qt restores and one full bootable restore")
    mode.add_argument("--ffu-qt-helper", action="store_true", help="Qualify XPRESS/full FFU together through ordinary Qt, one seed, readback and BIOS boot")
    parser.add_argument("--virtual-disk-chains", action="store_true", help="Combine VHD/VHDX parents, dirty-log restore and parent protections")
    mode.add_argument("--vhdx-qt-helper", action="store_true", help="Qualify dynamic/fixed VHDX together through ordinary Qt, one seed, native readback and BIOS boot")
    mode.add_argument("--vtsi-qt-helper", action="store_true", help="Qualify ordinary VTSI sparse SELECT/confirmation/READY and full bootable restore")
    mode.add_argument("--zip-qt-helper", action="store_true", help="Qualify ordinary ZIP SELECT/controller/confirmed single-helper deflate restore, joined READY/readback/BIOS and required seed/raw setup")
    mode.add_argument("--zstd-qt-helper", action="store_true", help="Qualify ordinary Zstd SELECT/controller/confirmed single-helper restore, joined READY/readback/BIOS and required seed/raw setup")
    mode.add_argument("--lzw-qt-helper", action="store_true", help="Qualify ordinary LZW and .gz fallback selection/controller/full restore, five real fallback faults, joined READY/readback/BIOS and required seed/raw setup")
    parser.add_argument("--lzw-encoder", type=Path, help="Pinned fixed ncompress encoder/decoder for --lzw-raw-helper; staged only in the disposable guest")
    mode.add_argument("--xz-qt-helper", action="store_true", help="Qualify ordinary gzip/XZ SELECT and confirmed controller creation, existing helper jobs and BIOS boot")
    mode.add_argument("--xz-raw-helper", action="store_true", help="Qualify XZ plus existing gzip helper jobs, actual guest block claims/readback and bootable restore")
    mode.add_argument("--compressed-raw-helper", action="store_true", help="Qualify gzip helper on a real guest disk, faults, claim lifetime and bootable restore")
    mode.add_argument("--mounted-raw-helper", action="store_true", help="Qualify single-launch mounted DD client/helper jobs")
    mode.add_argument("--mounted-raw", action="store_true", help="Qualify native raw writing after mounted-target acquisition; no DD helper/GUI integration")
    mode.add_argument("--mounted-helper", action="store_true", help="Qualify mounted-target acquisition and complete SFD/FAT32 helper jobs")
    mode.add_argument("--mounted-iso-helper", action="store_true", help="Qualify mounted partition acquisition and complete FAT32/Syslinux ISO creation")
    mode.add_argument("--sfd-exfat-helper", action="store_true", help="Qualify non-bootable whole-device exFAT helper jobs")
    mode.add_argument("--sfd-ntfs-helper", action="store_true", help="Qualify non-bootable whole-device NTFS helper jobs")
    mode.add_argument("--sfd-helper", action="store_true", help="Qualify a complete source-free SFD/FAT16 or FAT32 helper job")
    mode.add_argument("--sfd", action="store_true", help="Qualify non-bootable whole-device FAT16 or FAT32 formatter handoff")
    mode.add_argument("--exfat", action="store_true", help="Qualify GPT/exFAT partition formatting and claim handoff")
    mode.add_argument("--ntfs-helper", action="store_true", help="Qualify non-bootable GPT/NTFS without UEFI:NTFS")
    mode.add_argument("--mbr-ntfs-helper", action="store_true", help="Qualify non-bootable MBR/NTFS without UEFI:NTFS")
    mode.add_argument("--mbr-fat-helper", action="store_true", help="Qualify non-bootable MBR/FAT16 or FAT32 creation")
    mode.add_argument("--grub2-image", action="store_true", help="Qualify one compatible GRUB ISO through FAT16/FAT32/NTFS helper creation and BIOS Linux boot")
    mode.add_argument("--windows-bios", choices=("rufus", "win7"), help="Qualify native Windows BIOS/dual creation with generated metadata, without Setup/firmware boot")
    parser.add_argument("--windows-ntfs", choices=("ntfs", "lowntfs-3g"), help="Windows creation NTFS driver; fallback guest omits the kernel NTFS module")
    parser.add_argument("--windows-fat32", action="store_true", help="Use native FAT32 and original Windows PE records, without UEFI:NTFS")
    parser.add_argument("--windows-dual", action="store_true", help="Add x64 EFI files; NTFS also reserves/writes UEFI:NTFS")
    parser.add_argument("--windows-efi-repair", action="store_true", help="Supply EFI only inside install.wim and qualify the repaired fallback's x64 firmware handoff")
    parser.add_argument("--windows-version", type=int, choices=(7, 10, 11), default=11)
    parser.add_argument("--windows-udf", action="store_true", help="Use UDF bridge input for the generated Windows fixture")
    parser.add_argument("--windows-winpe", choices=("i386", "amd64", "minint"), help="Generated legacy setup instead of modern WIM metadata")
    parser.add_argument("--windows-winpe-partial-minint", action="store_true", help="Use /minint with complete i386/amd64 and only an ancillary minint SIF")
    parser.add_argument("--windows-winpe-syslinux", choices=("create", "keep", "overwrite"), help="Copy Syslinux configs through the legacy FAT32/NTFS Alt+E job, optionally using Widgets")
    parser.add_argument("--windows-winpe-grub-data", choices=("grub2", "grub4dos", "both"), help="Add GRUB data to WinPE; qualify its independent SBR on FAT32/NTFS without Syslinux")
    parser.add_argument("--windows-winpe-failure", action="store_true", help="Fail the first prepared root file write, then check failed completion and cleanup")
    parser.add_argument("--windows-winpe-gui", action="store_true", help="Use the existing Widgets guest to create the generated legacy USB")
    parser.add_argument("--windows-winpe-root-detector", action="store_true", help="Keep a mixed-case root NTDETECT.COM in the generated legacy source")
    parser.add_argument("--windows-winpe-root-detector-content", choices=("identical", "different"), default="identical", help="Root detector content; identical preserves the original fixture")
    parser.add_argument("--windows-winpe-root-sif", action="store_true", help="Include a distinct mixed-case root TXTSETUP.SIF in the generated legacy source")
    parser.add_argument("--windows-winpe-root-loader", action="store_true", help="Prepare a mixed-case root BOOTMGR in the generated legacy source")
    parser.add_argument("--windows-winpe-root-loader-content", choices=("identical", "prepatched"), default="identical", help="Root loader content; identical preserves the original fixture")
    parser.add_argument("--windows-boot", action="store_true", help="Qualify generated BOOTMGR BIOS handoff, Rufus MBR prompt and optional x64 UEFI handoff")
    parser.add_argument("--grub-modules", type=Path, help="Pinned i386-pc module directory for --grub2-image")
    parser.add_argument("--grub-ntfs", choices=("ntfs", "lowntfs-3g"), help="GRUB ISO NTFS creation with the preferred kernel driver or absent-kernel fallback")
    parser.add_argument("--grub-uefi", action="store_true", help="Qualify required UEFI:NTFS and x64 handoff with --grub-ntfs")
    parser.add_argument("--grub-core", type=Path, help="Pinned Rufus core.img for --grub2-image")
    mode.add_argument("--grub4dos-gui", action="store_true", help="Create standalone GRUB4DOS FAT16/FAT32/NTFS through Qt, then qualify native cleanup/readback and BIOS execution")
    mode.add_argument("--grub4dos-helper", action="store_true", help="Qualify standalone GRUB4DOS FAT16/FAT32 loader copy and native helper completion")
    mode.add_argument("--grub4dos-image", action="store_true", help="Qualify plain BIOS/FAT or NTFS ISO creation with the image's own GRUB4DOS loader and menu")
    parser.add_argument("--grub4dos-image-gui", action="store_true", help="Select --grub4dos-image through ordinary Qt ISO loading before native creation/readback/BIOS")
    parser.add_argument("--grub4dos-image-uefi", action="store_true", help="Qualify combined BIOS/x64 UEFI FAT16/FAT32 GRUB4DOS ISO creation and original source fallback")
    parser.add_argument("--grub4dos-ntfs", action="store_true", help="Use NTFS for GRUB4DOS standalone or ISO helper/GUI creation")
    parser.add_argument("--grub4dos-ntfs-driver", choices=("ntfs", "lowntfs-3g"), help="Select the native qualification provider; fallback guest omits the kernel NTFS module")
    mode.add_argument("--grub2-helper", action="store_true", help="Qualify standalone MBR/FAT16 or FAT32 GRUB2 helper jobs and bare BIOS boot")
    mode.add_argument("--grub2-ntfs-helper", action="store_true", help="Qualify standalone MBR/NTFS GRUB2 helper and bare BIOS boot")
    mode.add_argument("--grub2-ext-gui", action="store_true", help="Create a virtual GRUB2/ext USB through Qt Widgets and the real helper")
    mode.add_argument("--grub2-ext-helper", action="store_true", help="Qualify standalone MBR/ext2 or ext3 GRUB2 helper and bare BIOS boot")
    mode.add_argument("--fat-helper", action="store_true", help="Qualify the complete non-bootable GPT/FAT16 or FAT32 job")
    mode.add_argument("--mbr-exfat-helper", action="store_true", help="Qualify non-bootable MBR/exFAT creation")
    mode.add_argument("--exfat-helper", action="store_true", help="Qualify the complete non-bootable GPT/exFAT client and helper job")
    mode.add_argument("--freedos-helper", action="store_true", help="Qualify the complete FreeDOS helper on virtual USB")
    for major in (4, 6):
        mode.add_argument(f"--syslinux{major}-helper", dest="syslinux_major", action="store_const", const=major, default=0,
                          help=f"Qualify standalone Syslinux {major} helper jobs and bare BIOS boot without an ISO")
    parser.add_argument("--freedos-boot", action="store_true", help="BIOS-boot the completed FreeDOS USB and execute DOS commands")
    parser.add_argument("--freedos-win7", action="store_true", help="Use the Windows 7 fallback MBR and US/437 startup in FreeDOS qualification")
    mode.add_argument("--handoff", action="store_true", help="Test real FAT formatting/mount handoffs instead of table preparation")
    mode.add_argument("--install", action="store_true", help="Qualify the native installer while retaining the exclusive disk claim")
    mode.add_argument("--helper", action="store_true", help="Run the complete production helper through its pipe protocol")
    mode.add_argument("--alpine", type=Path, help="Qualify the pinned real Alpine ISO through the production Syslinux helper")
    parser.add_argument("--syslinux-ntfs", choices=("ntfs", "lowntfs-3g"), help="Standalone v6 NTFS helper, with preferred kernel or absent-kernel fallback")
    parser.add_argument("--syslinux-image", action="store_true", help="Qualify image jobs with --syslinux6-helper --syslinux-ntfs")
    parser.add_argument("--mkisofs", type=Path, help="Required ISO fixture builder for --syslinux-image")
    parser.add_argument("--syslinux-uefi", action="store_true", help="Add and boot the required UEFI:NTFS partition with --syslinux-image")
    parser.add_argument("--fat-bits", type=int, choices=(16, 32), default=32)
    parser.add_argument("--ext-bits", type=int, choices=(2, 3), default=3)
    parser.add_argument("--upstream", type=Path, help="Qualified Syslinux release source matching the selected provider")
    parser.add_argument("--installer", type=Path, help="Qualified native executable; default: matching provider in BUILD/syslinux/")
    parser.add_argument("--ntfs-3g-package", type=Path, help="NTFS-3G package, unpacked only in the guest")
    parser.add_argument("--ntfs-library-package", type=Path, help="Matching libntfs-3g package for the guest")
    parser.add_argument("output", type=Path, help="New output directory; must not exist")
    args = parser.parse_args()
    if args.udf_indexing_batch:
        args.native_formats = True
    if args.geometry_batch:
        args.handoff = True
        if not args.mkudffs: parser.error('--geometry-batch requires --mkudffs')
        args.mkudffs = args.mkudffs.resolve(strict=True)
    if args.legacy_iso:
        args.handoff = True
        if not args.mkisofs: parser.error('--legacy-iso requires --mkisofs')
        args.mkisofs = args.mkisofs.resolve(strict=True)
    if args.iso_resources:
        args.handoff = True
        if not args.mkisofs: parser.error('--iso-resources requires --mkisofs')
        args.mkisofs = args.mkisofs.resolve(strict=True)
    if args.device_source: args.device_options = True
    if args.device_options: args.handoff = True
    if args.ntfs_options:
        args.handoff = True
        if not args.mkisofs: parser.error('--ntfs-options requires --mkisofs')
        args.mkisofs = args.mkisofs.resolve(strict=True)
    if args.iso_options:
        args.handoff = True
        if not args.mkisofs or not args.msdos_resource: parser.error('--iso-options requires --mkisofs and --msdos-resource')
        args.mkisofs = args.mkisofs.resolve(strict=True)
        args.msdos_resource = args.msdos_resource.resolve(strict=True)
    if args.legacy_boot:
        args.handoff = True
        if not args.msdos_resource: parser.error('--legacy-boot requires --msdos-resource')
        args.msdos_resource = args.msdos_resource.resolve(strict=True)
    if args.runtime_validation:
        args.handoff = True
        if not args.mkisofs: parser.error('--runtime-validation requires --mkisofs')
        args.mkisofs = args.mkisofs.resolve(strict=True)
    if args.native_formats:
        args.handoff = True
        if not args.mkudffs: parser.error('--native-formats requires --mkudffs')
        args.mkudffs = args.mkudffs.resolve(strict=True)
    if args.lzw_raw_helper or args.lzw_qt_helper:
        if args.lzw_encoder is None:
            parser.error("--lzw-raw-helper or --lzw-qt-helper requires --lzw-encoder")
        args.lzw_encoder = args.lzw_encoder.resolve(strict=True)
    elif args.lzw_encoder is not None:
        parser.error("--lzw-encoder requires --lzw-raw-helper or --lzw-qt-helper")
    args.qt_gui = args.grub2_ext_gui or args.windows_winpe_gui or args.grub4dos_gui or args.grub4dos_image_gui
    args.vtsi_raw_helper = args.vtsi_raw_helper or args.vtsi_qt_helper
    if args.grub4dos_image_gui and not args.grub4dos_image:
        parser.error("--grub4dos-image-gui requires --grub4dos-image")
    if args.grub4dos_image_uefi:
        if not args.grub4dos_image:
            parser.error("--grub4dos-image-uefi requires --grub4dos-image")
    if args.windows_winpe_grub_data:
        native_grub4dos = args.windows_winpe_grub_data == "grub4dos" and not args.windows_winpe_gui
        if not args.windows_winpe_gui and not native_grub4dos:
            parser.error("GRUB2/data qualification requires --windows-winpe-gui")
        if not args.windows_winpe_syslinux:
            sbr_filesystem = args.windows_ntfs or args.windows_fat32
            qualified_sbr = args.windows_bios and args.windows_winpe and sbr_filesystem
            if not qualified_sbr:
                parser.error("GRUB SBR qualification requires Windows BIOS, FAT32 or NTFS and a WinPE fixture")
    if args.windows_winpe_syslinux:
        has_filesystem = args.windows_fat32 or args.windows_ntfs is not None
        if not args.windows_winpe or not args.windows_bios or not has_filesystem:
            parser.error("--windows-winpe-syslinux requires --windows-bios, --windows-winpe and --windows-fat32 or --windows-ntfs")
        if args.windows_winpe_failure:
            parser.error("--windows-winpe-syslinux does not qualify root-write faults")
    if args.windows_winpe_partial_minint and args.windows_winpe not in ("i386", "amd64"):
        parser.error("--windows-winpe-partial-minint requires an i386 or amd64 fixture")
    if args.windows_winpe_root_detector_content == "different" and not args.windows_winpe_root_detector:
        parser.error("--windows-winpe-root-detector-content different requires --windows-winpe-root-detector")
    if args.windows_winpe_root_loader_content == "prepatched":
        if not args.windows_winpe_root_loader:
            parser.error("--windows-winpe-root-loader-content prepatched requires --windows-winpe-root-loader")
        if args.windows_winpe not in ("i386", "amd64") or args.windows_winpe_partial_minint:
            parser.error("Prepatched root loaders require an i386 or amd64 patch-loader fixture")
    if args.grub_ntfs and not args.grub2_image:
        parser.error("--grub-ntfs requires --grub2-image")
    if args.grub_ntfs and args.fat_bits != 32:
        parser.error("--grub-ntfs cannot be combined with --fat-bits 16")
    if args.grub_uefi and not args.grub_ntfs:
        parser.error("--grub-uefi requires --grub2-image and --grub-ntfs")
    if args.windows_bios:
        if args.windows_winpe:
            incompatible_legacy = args.windows_dual or args.windows_efi_repair or args.windows_boot
            if incompatible_legacy:
                parser.error("--windows-winpe requires NTFS or FAT32 BIOS, without modern/EFI fixture options")
            if args.windows_winpe_gui and args.windows_winpe_failure:
                parser.error("--windows-winpe-gui does not inject helper faults")
        elif args.windows_winpe_failure:
            parser.error("--windows-winpe-failure requires --windows-winpe")
        elif args.windows_winpe_gui:
            parser.error("--windows-winpe-gui requires --windows-winpe")
        if args.windows_winpe_root_detector and not args.windows_winpe:
            parser.error("--windows-winpe-root-detector requires --windows-winpe")
        if args.windows_winpe_root_sif and not args.windows_winpe:
            parser.error("--windows-winpe-root-sif requires --windows-winpe")
        if args.windows_winpe_root_loader and not args.windows_winpe:
            parser.error("--windows-winpe-root-loader requires --windows-winpe")
        if bool(args.windows_ntfs) == args.windows_fat32 or not args.mkisofs:
            parser.error("--windows-bios requires exactly one of --windows-ntfs/--windows-fat32, and --mkisofs")
        if args.fat_bits != 32:
            parser.error("--windows-bios supports FAT32 or NTFS, not FAT16")
        older_enhanced = args.windows_version == 7 and (args.windows_fat32 or args.windows_dual)
        if older_enhanced and not args.windows_efi_repair:
            parser.error("The older enhanced fixture currently requires --windows-efi-repair")
        if args.windows_efi_repair and not args.windows_dual:
            parser.error("--windows-efi-repair requires --windows-dual")
        if args.windows_efi_repair and args.windows_boot:
            parser.error("--windows-efi-repair qualifies EFI only; omit --windows-boot")
        args.handoff = True
    elif args.windows_ntfs or args.windows_fat32 or args.windows_dual or args.windows_efi_repair or args.windows_udf or args.windows_boot or args.windows_winpe or args.windows_winpe_failure or args.windows_winpe_gui or args.windows_winpe_root_detector or args.windows_winpe_root_sif or args.windows_winpe_root_loader or args.windows_version != 11:
        parser.error("Windows fixture options require --windows-bios")
    args.ntfs_mount_driver = args.windows_ntfs or args.grub_ntfs or args.syslinux_ntfs or ((args.grub4dos_ntfs_driver or "ntfs") if args.grub4dos_ntfs else None)
    if args.syslinux_ntfs and args.syslinux_major != 6:
        parser.error("--syslinux-ntfs requires --syslinux6-helper")
    if args.syslinux_image and (not args.syslinux_ntfs or not args.mkisofs):
        parser.error("--syslinux-image requires --syslinux-ntfs and --mkisofs")
    if args.syslinux_uefi and not args.syslinux_image:
        parser.error("--syslinux-uefi requires --syslinux-image")
    args.installer_name = "rufus-extlinux" if args.syslinux_ntfs else "rufus-syslinux4" if args.syslinux_major == 4 else "rufus-syslinux"
    args.standalone_uefi = args.uefi_ntfs_helper or args.uefi_exfat_helper
    if args.uefi_mbr and not args.standalone_uefi:
        parser.error("--uefi-mbr requires --uefi-ntfs-helper or --uefi-exfat-helper")
    if args.grub4dos_ntfs_driver and not args.grub4dos_ntfs:
        parser.error("--grub4dos-ntfs-driver requires --grub4dos-ntfs")
    if args.grub4dos_ntfs and not (args.grub4dos_helper or args.grub4dos_gui or args.grub4dos_image):
        parser.error("--grub4dos-ntfs requires a GRUB4DOS standalone or image route")
    if args.grub4dos_ntfs and args.fat_bits != 32:
        parser.error("--fat-bits does not select an NTFS cluster size; omit it for NTFS")
    args.mbr_ntfs_helper = args.mbr_ntfs_helper or args.grub4dos_ntfs or args.grub2_ntfs_helper or (args.uefi_mbr and args.uefi_ntfs_helper)
    args.mbr_exfat_helper = args.mbr_exfat_helper or (args.uefi_mbr and args.uefi_exfat_helper)
    packages_given = args.ntfs_3g_package is not None or args.ntfs_library_package is not None
    packages_complete = args.ntfs_3g_package is not None and args.ntfs_library_package is not None
    if (args.ntfs_mounted_helper or args.ntfs_mount_driver or args.save_export or args.media_appearance or args.ntfs_options or args.iso_resources or args.legacy_iso or args.geometry_batch) and not packages_complete:
        parser.error("--ntfs-mounted-helper and --syslinux-ntfs require both NTFS-3G and matching library packages (also required by --grub-ntfs)")
    if packages_given and not (args.ntfs_mounted_helper or args.ntfs_mount_driver or args.save_export or args.media_appearance or args.ntfs_options or args.iso_resources or args.legacy_iso or args.geometry_batch):
        parser.error("NTFS-3G packages require an NTFS mount qualification")
    if args.ntfs_mounted_helper or args.ntfs_mount_driver or args.save_export or args.media_appearance or args.ntfs_options or args.iso_resources or args.legacy_iso or args.geometry_batch:
        args.ntfs_3g_package = args.ntfs_3g_package.resolve(strict=True)
        args.ntfs_library_package = args.ntfs_library_package.resolve(strict=True)
        if args.ntfs_mounted_helper: args.mounted_raw_helper = True
    args.vhd_raw_helper = args.vhd_raw_helper or args.vhd_qt_helper
    args.ffu_geometry = args.ffu_geometry or args.ffu_layouts
    if sum((bool(args.ffu_optimized), args.ffu_stores, args.ffu_geometry, args.ffu_variants, args.ffu_partial, args.image_entry, args.zero_device, args.full_format, args.bad_blocks, args.media_appearance, args.old_bios)) > 1:
        parser.error("Choose one combined FFU qualification route")
    if args.ffu_catalog and not args.ffu_stores:
        parser.error("--ffu-catalog requires --ffu-stores")
    if args.ffu_optimized:
        args.ffu_optimized = args.ffu_optimized.resolve(strict=True)
        for name in ("smaller.ffu", "larger.ffu", "cases.json"):
            if not (args.ffu_optimized / name).is_file(): parser.error("Missing optimized FFU fixture: " + name)
    args.ffu_qt_helper = args.ffu_qt_helper or bool(args.ffu_optimized) or args.ffu_stores or args.ffu_variants or args.ffu_geometry or args.ffu_partial or args.image_entry or args.zero_device or args.full_format or args.bad_blocks or args.media_appearance or args.old_bios
    args.vhdx_qt_helper = args.vhdx_qt_helper or args.virtual_disk_chains
    args.mounted_raw_helper = args.mounted_raw_helper or args.vhdx_qt_helper or args.ffu_qt_helper or args.save_qt_helper
    if args.bzip2_qt_helper or args.lzma_raw_helper or args.lzma_qt_helper or args.lzw_raw_helper or args.lzw_qt_helper or args.zip_raw_helper or args.zip_qt_helper or args.zstd_raw_helper or args.zstd_qt_helper or args.vtsi_raw_helper or args.vtsi_qt_helper or args.vhd_raw_helper:
        args.mounted_raw_helper = True
    if args.bzip2_raw_helper:
        args.xz_qt_helper = True
    if args.xz_qt_helper:
        args.xz_raw_helper = args.compressed_qt_helper = True
    if (args.mounted_helper or args.mounted_iso_helper or args.mounted_raw or args.mounted_raw_helper or args.fixed_usb_helper or args.compressed_raw_helper or args.compressed_qt_helper or args.xz_raw_helper) and args.fat_bits != 32:
        parser.error("Mounted helper recipes currently qualify FAT32 only")
    args.sfd_helper = args.sfd_helper or args.mounted_helper
    args.compressed_raw_helper = args.compressed_raw_helper or args.compressed_qt_helper or args.xz_raw_helper
    args.mounted_raw_helper = args.mounted_raw_helper or args.fixed_usb_helper or args.compressed_raw_helper
    args.helper = args.helper or args.mounted_iso_helper or args.mounted_raw or args.mounted_raw_helper or args.zip_supplement
    if args.windows_togo:
        args.handoff = True
        if not args.mkisofs: parser.error('--windows-togo requires --mkisofs')
    if args.grub2_image:
        if not args.grub_modules or not args.grub_core or not args.mkisofs:
            parser.error("--grub2-image requires --grub-modules, --grub-core and --mkisofs")
        args.grub_modules = args.grub_modules.resolve(strict=True)
        args.grub_core = args.grub_core.resolve(strict=True)
        args.grub2_helper = True
    if args.grub4dos_image:
        if not args.mkisofs:
            parser.error("--grub4dos-image requires --mkisofs")
        args.fat_helper = not args.grub4dos_ntfs
    args.grub4dos_helper = args.grub4dos_helper or args.grub4dos_gui
    args.grub2_ext_helper = args.grub2_ext_helper or args.grub2_ext_gui
    args.build = args.build.resolve()
    if args.grub4dos_image_uefi and not (args.build / "uefi-handoff.efi").is_file():
        parser.error("--grub4dos-image-uefi requires the built uefi_handoff_fixture")
    args.fat_helper = args.fat_helper or args.mbr_fat_helper or args.sfd_helper or args.grub2_helper or (args.grub4dos_helper and not args.grub4dos_ntfs)
    args.ntfs_helper = args.ntfs_helper or args.mbr_ntfs_helper or args.sfd_ntfs_helper or args.uefi_ntfs_helper
    args.exfat_helper = args.exfat_helper or args.mbr_exfat_helper or args.sfd_exfat_helper or args.uefi_exfat_helper
    if args.freedos_win7 and not args.freedos_helper:
        parser.error("--freedos-win7 requires --freedos-helper")
    if args.freedos_boot and not args.freedos_helper:
        parser.error("--freedos-boot requires --freedos-helper")
    if args.syslinux_major or args.freedos_helper or args.sfd or args.exfat or args.exfat_helper or args.fat_helper or args.ntfs_helper or args.grub2_ext_helper:
        args.handoff = True
    if args.alpine:
        args.alpine = args.alpine.resolve()
        args.helper = True
    if (args.install or args.helper or args.syslinux_major) and args.upstream is None:
        parser.error("Syslinux installation/helper qualification requires --upstream")
    if args.fat_bits == 16 and (args.alpine or not (args.handoff or args.install or args.helper)):
        parser.error("FAT16 qualification requires --handoff, --install or --helper")
    base = args.output.resolve()
    if any(char in str(base) for char in ",\n\r"):
        parser.error("Output path must not contain QEMU option separators")
    name = "test_mbr_handoff_guest" if args.handoff or args.install or args.helper else "test_mbr_guest"
    args.binaries = [args.build / name]
    if args.qt_gui:
        args.binaries.append(args.build / "test_grub2_widgets_guest")
        args.binaries.append(Path("/usr/lib/qt6/plugins/platforms/libqoffscreen.so"))
    if args.handoff or args.install or args.helper:
        args.binaries.extend([args.build / "udisks_fixture", Path("/usr/bin/dbus-daemon"), Path("/usr/bin/mkfs.fat")])
    if args.ntfs_helper or args.standalone_uefi or args.syslinux_uefi or args.grub_uefi or (args.windows_dual and not args.windows_fat32) or args.legacy_iso:
        args.boot_payload = prepare.HERE.parents[2] / "res/uefi/uefi-ntfs.img"
    if args.zip_supplement:
        args.binaries.append(args.build / "test_archive_qt_guest")
    if args.iso_resources:
        args.binaries.extend([args.build/'syslinux/rufus-syslinux',args.build/'syslinux/rufus-syslinux4',
            args.build/'syslinux/rufus-extlinux',Path('/usr/bin/ntfsinfo'),Path('/usr/bin/mcopy'),Path('/usr/bin/mattrib')])
    if args.legacy_iso:
        args.binaries.extend([args.build/'test_native_format_qt_guest',args.build/'rufus-prepare-helper',
            args.build/'syslinux/rufus-syslinux',args.build/'syslinux/rufus-syslinux4',
            Path('/usr/bin/mcopy'),Path('/usr/bin/mattrib'),Path('/usr/bin/ntfsinfo')])
        for name in ('uefi-handoff.efi', 'wim_fixture'):
            if not (args.build/name).is_file(): parser.error('Build the ' + name + ' fixture first')
    if args.device_source:
        args.binaries.extend([args.build / "test_device_source_qt_guest", Path('/usr/bin/qemu-nbd'),
            Path('/usr/bin/mkfs.btrfs'), Path('/usr/bin/losetup'), Path('/usr/bin/ln'), Path('/usr/bin/mkdir'), Path('/usr/bin/setpriv')])
    if args.device_options:
        args.binaries.extend([args.build/"test_native_format_qt_guest", args.build/"rufus-prepare-helper"])
    if args.legacy_boot:
        args.binaries.extend([args.build/'test_native_format_qt_guest',args.build/'rufus-prepare-helper'])
    if args.runtime_validation or args.iso_options or args.ntfs_options or args.iso_resources:
        args.binaries.extend([args.build / "test_native_format_qt_guest", args.build / "rufus-prepare-helper", Path("/usr/bin/mkntfs")])
    if args.native_formats:
        args.binaries.extend([args.build / "test_native_format_qt_guest", args.build / "rufus-prepare-helper", Path("/usr/bin/mke2fs"), args.mkudffs])
    if args.geometry_batch:
        args.binaries.extend([args.build / 'test_native_format_qt_guest', args.build / 'rufus-prepare-helper',
            Path('/usr/bin/mke2fs'), Path('/usr/bin/mkntfs'), Path('/usr/bin/mkfs.exfat'), args.mkudffs])
        if not (args.build / 'uefi-handoff.efi').is_file(): parser.error('Build the uefi-handoff.efi fixture first')
    if args.windows_togo:
        args.binaries.extend([args.build / 'test_windows_to_go_qt_guest', args.build / 'rufus-prepare-helper', Path('/usr/bin/mkntfs')])
    if args.media_appearance:
        args.binaries.extend([Path("/usr/bin/mkntfs"), Path("/usr/bin/mkfs.exfat"), args.build / "test_media_appearance_qt_guest"])
    if args.old_bios:
        args.binaries.extend([Path("/usr/bin/mkntfs"), Path("/usr/bin/mke2fs"), Path("/usr/bin/e2fsck"),
            args.build / "test_media_appearance_qt_guest"])
    if args.full_format:
        args.binaries.extend([Path("/usr/bin/mkntfs"), Path("/usr/bin/mkfs.exfat"), Path("/usr/bin/mke2fs"), Path("/usr/bin/debugfs"), Path("/usr/bin/e2fsck")])
    if args.ntfs_helper:
        args.binaries.append(Path("/usr/bin/mkntfs"))
    if args.grub2_ext_helper:
        args.binaries.append(Path("/usr/bin/mke2fs"))
    if args.ntfs_mounted_helper or args.ntfs_mount_driver or args.save_export or args.media_appearance or args.ntfs_options or args.iso_resources or args.legacy_iso or args.geometry_batch:
        args.binaries.append(Path("/usr/bin/mkntfs"))
    if args.exfat or args.exfat_helper:
        args.binaries.append(Path("/usr/bin/mkfs.exfat"))
    if args.windows_bios:
        args.binaries.append(args.build / "rufus-prepare-helper")
        if args.windows_winpe_failure:
            args.binaries.append(args.build / "prepare_extract_probe")
    if args.windows_bios or args.exfat_helper or args.fat_helper or args.ntfs_helper or args.grub2_ext_helper:
        if not args.windows_bios:
            args.binaries.extend([args.build / "rufus-prepare-helper", args.build / "prepare_exfat_probe"])
        flags = (args.build / "build.ninja").read_text()
        if args.exfat_helper and 'RUFUS_MKFS_EXFAT_PATH=\\"/usr/bin/mkfs.exfat\\"' not in flags:
            parser.error("This guest recipe requires mkfs.exfat in /usr/bin")
        if (args.fat_helper or args.windows_fat32) and 'RUFUS_MKFS_FAT_PATH=\\"/usr/bin/mkfs.fat\\"' not in flags:
            parser.error("This guest recipe requires mkfs.fat in /usr/bin")
        windows_ntfs = args.windows_bios and not args.windows_fat32
        if (args.ntfs_helper or args.grub_ntfs or windows_ntfs) and 'RUFUS_MKNTFS_PATH=\\"/usr/bin/mkntfs\\"' not in flags:
            parser.error("This guest recipe requires mkntfs in /usr/bin")
        if args.grub2_ext_helper and 'RUFUS_MKE2FS_PATH=\\"/usr/bin/mke2fs\\"' not in flags:
            parser.error("This guest recipe requires mke2fs in /usr/bin")
    if args.freedos_helper:
        args.binaries.extend([args.build / "rufus-prepare-helper", args.build / "prepare_extract_probe"])
    if args.syslinux_major:
        args.binaries.extend([args.build / "prepare_syslinux_probe", Path("/usr/bin/true"), Path("/usr/bin/false")])
        sys.path.append(str(prepare.HERE.parents[1] / "probes/syslinux"))
    if args.install or args.helper or args.syslinux_major:
        args.upstream = args.upstream.resolve()
        installer = args.installer.resolve() if args.installer else args.build / "syslinux" / args.installer_name
        if installer.name != args.installer_name:
            parser.error(f"Native executable must be named {args.installer_name}")
        args.binaries.extend([installer, Path("/usr/bin/mcopy"), Path("/usr/bin/mattrib")])
    if args.xz_raw_helper or args.lzma_raw_helper or args.lzma_qt_helper or args.zip_raw_helper:
        args.binaries.append(Path("/usr/bin/xz"))
    if args.bzip2_raw_helper or args.bzip2_qt_helper or args.zip_raw_helper:
        args.binaries.append(Path("/usr/bin/bzip2"))
    if args.zstd_raw_helper or args.zstd_qt_helper:
        args.binaries.append(Path("/usr/bin/zstd"))
    if args.compressed_qt_helper or args.bzip2_qt_helper or args.lzma_qt_helper or args.lzw_qt_helper or args.zip_qt_helper or args.zstd_qt_helper or args.vtsi_qt_helper or args.vhd_qt_helper:
        args.binaries.append(args.build / "test_compressed_qt_guest")
    if args.save_read_only and not args.save_export:
        parser.error("--save-read-only requires --save-export")
    if args.save_export:
        if not args.save_qt_helper:
            parser.error("--save-export requires --save-qt-helper")
        import re as cache_re
        cache_text = (args.build / 'CMakeCache.txt').read_text()
        args.export_encoder = Path(cache_re.search(r'^RUFUS_MKISOFS:FILEPATH=(.+)$', cache_text, cache_re.M)[1])
        args.binaries.extend([args.build / 'rufus-save-guest-helper', args.build / 'save_guest_launcher', args.export_encoder, Path('/usr/bin/mkdir'), Path('/usr/bin/chmod'), Path('/usr/bin/ln'), Path('/usr/bin/mkntfs')])
    if args.save_qt_helper:
        args.binaries.extend([args.build / "test_compressed_qt_guest", args.build / "test_image_save_qt_guest"])
    if args.ffu_qt_helper:
        args.binaries.append(args.build / "test_compressed_qt_guest")
    if args.vhdx_qt_helper:
        args.binaries.extend([args.build / "test_compressed_qt_guest", Path("/usr/bin/qemu-img")])
    if args.media_appearance or args.bad_blocks or args.compressed_raw_helper or args.lzma_raw_helper or args.lzw_raw_helper or args.lzw_qt_helper or args.zip_raw_helper or args.zstd_raw_helper or args.vtsi_raw_helper or args.vhd_raw_helper:
        args.binaries.append(args.build / "compressed_helper_guest_probe")
    if args.lzw_raw_helper or args.lzw_qt_helper:
        args.binaries.append(args.lzw_encoder)
    if args.helper or args.syslinux_major:
        args.binaries.append(args.build / "rufus-prepare-helper")
        flags = (args.build / "build.ninja").read_text()
        definition = "RUFUS_NATIVE_EXTLINUX_PATH" if args.syslinux_ntfs else "RUFUS_NATIVE_SYSLINUX4_PATH" if args.syslinux_major == 4 else "RUFUS_NATIVE_SYSLINUX_PATH"
        match = re.search(definition + r'=\\"([^\\"]+)\\"', flags)
        if match is None:
            parser.error("Cannot find the helper's compiled native installer location")
        args.installer_path = match[1]
        if 'RUFUS_MTOOLS_DIRECTORY=\\"/usr/bin\\"' not in flags:
            parser.error("This guest recipe requires mtools in /usr/bin")
    if args.windows_bios and not (args.build / "wim_fixture").is_file():
        parser.error("Build the wim_fixture CMake target first")
    if args.windows_boot:
        for fixture in ("windows-bootmgr.bin", "windows-second-disk.bin"):
            if not (args.build / fixture).is_file():
                parser.error("Build the windows_bios_handoff_fixture CMake target first")
    if (args.windows_bios or args.syslinux_uefi or args.grub_uefi) and not (args.build / "uefi-handoff.efi").is_file():
        parser.error("Build the uefi_handoff_fixture CMake target first")
    if args.ffu_optimized:
        args.binaries.extend([args.build / "ntfs_file_fixture", Path("/usr/bin/setpriv"), Path("/usr/bin/ntfsresize")])
    for binary in args.binaries:
        if not binary.is_file():
            parser.error(f"Missing {binary}; build guest targets with RUFUS_BUILD_VM_TEST=ON first")
    base.mkdir()
    try:
        if args.windows_bios:
            if args.windows_winpe:
                from winpe_helper import make_fixture
            else:
                from windows_bios import make_fixture
            make_fixture(args, base)
        if args.grub2_image:
            from grub2_iso import make_fixture
            make_fixture(args, base)
        if args.grub4dos_image:
            make_grub4dos_image_fixture(args, base)
        if args.syslinux_image:
            make_ntfs_image_fixture(args, base)
        elif args.alpine:
            from syslinux_alpine import prepare_source
            prepare_source(args, base)
        elif args.install or args.helper:
            make_install_fixture(args, base)
        make_guest(args, base)
        inputs = [base / "vmlinuz", base / "initramfs.cpio", *args.binaries]
        if args.iso_resources:
            inputs.extend([args.mkisofs,args.build/'uefi-handoff.efi',prepare.HERE/'iso_resources.py'])
        if args.legacy_iso:
            inputs.extend([args.mkisofs,args.build/'uefi-handoff.efi',args.build/'wim_fixture',
                prepare.HERE/'legacy_iso.py',prepare.HERE/'legacy_iso_multiboot.asm',prepare.HERE/'legacy_iso_kolibri.asm'])
        if args.geometry_batch:
            inputs.extend([args.mkudffs, args.build / 'uefi-handoff.efi', prepare.HERE / 'geometry_batch.py'])
        if args.ntfs_options:
            inputs.extend([args.mkisofs, args.build / 'uefi-handoff.efi', args.build / 'windows_to_go_fixture', prepare.HERE / 'ntfs_options.py'])
        if args.iso_options:
            inputs.extend([args.msdos_resource, args.mkisofs, args.build / 'uefi-handoff.efi', prepare.HERE / 'iso_options.py', prepare.HERE / 'legacy_boot.py'])
        if args.legacy_boot:
            inputs.extend([args.msdos_resource,prepare.HERE/"legacy_boot.py",prepare.HERE/"legacy_freeldr.asm"])
        if args.runtime_validation:
            inputs.extend([args.mkisofs, args.build / 'uefi-handoff.efi', prepare.HERE / 'runtime_validation.py'])
        if args.zip_supplement:
            inputs.extend([base / "supplement.zip", base / "bad-supplement.zip", prepare.HERE / "zip_supplement.py"])
        if args.lzw_raw_helper or args.lzw_qt_helper:
            inputs.extend(sorted((base / 'initramfs/lzw-fixtures').iterdir()))
            inputs.extend([base / 'lzw-fixtures.json', prepare.HERE / 'lzw_fixtures.py', prepare.HERE.parent / 'lzw_reference.py'])
        if args.grub4dos_helper:
            inputs.append(prepare.HERE.parent / "fixtures/download/grub4dos-0.4.6a/grldr")
            if args.grub4dos_ntfs:
                inputs.extend([args.build / "ntfs_file_fixture", Path("/usr/bin/7z")])
        if args.grub4dos_image:
            inputs.extend([args.iso, args.mkisofs, prepare.HERE.parent / "fixtures/download/grub4dos-0.4.6a/grldr"])
            if args.grub4dos_image_uefi:
                inputs.append(args.build / "uefi-handoff.efi")
            if args.grub4dos_ntfs:
                inputs.extend([args.build / "ntfs_file_fixture", Path("/usr/bin/7z")])
        if args.windows_bios:
            inputs.extend([args.iso, args.build / "wim_fixture", args.build / "uefi-handoff.efi"])
        if args.grub2_image:
            inputs.extend([args.iso, args.grub_core, args.grub_modules / "normal.mod"])
        if args.ntfs_mounted_helper or args.ntfs_mount_driver or args.save_export or args.media_appearance or args.ntfs_options or args.iso_resources or args.legacy_iso or args.geometry_batch:
            inputs.extend([args.ntfs_3g_package, args.ntfs_library_package, Path('/usr/lib/libfuse.so.2')])
        if args.sfd or args.sfd_helper:
            inputs.extend([Path("/usr/bin/fsck.fat"), Path("/usr/bin/mdir"), Path("/usr/bin/sfdisk")])
        if args.exfat or args.exfat_helper:
            inputs.extend([Path("/usr/bin/fsck.exfat"), Path("/usr/bin/dump.exfat")])
        if args.ntfs_helper or args.grub_ntfs or (args.windows_bios and not args.windows_fat32):
            inputs.extend([Path("/usr/bin/ntfsinfo"), Path("/usr/bin/ntfslabel"), Path("/usr/bin/ntfsck")])
        if args.grub2_ext_helper:
            inputs.extend([Path("/usr/bin/e2fsck"), Path("/usr/bin/dumpe2fs")])
        if args.ntfs_helper or args.standalone_uefi or args.syslinux_uefi or args.grub_uefi or (args.windows_dual and not args.windows_fat32) or args.legacy_iso:
            inputs.append(args.boot_payload)
        if args.grub_uefi:
            inputs.append(args.build / "uefi-handoff.efi")
        if args.syslinux_uefi:
            inputs.extend([args.build / "uefi-handoff.efi", base / "bios.iso"])
        if args.install or args.helper:
            inputs.append(args.iso)
        if args.syslinux_image:
            inputs.extend([args.iso, args.build / 'syslinux_scan_probe', args.build / 'iso_write_check_probe'])
            inputs.extend(base / (name + '.iso') for name in ('unknown', 'incompatible', 'conflict', 'efi'))
        if args.helper and not args.alpine:
            inputs.extend([base / "unknown/source.iso", base / "incompatible/source.iso"])
        manifest = {"windows_fat32": args.windows_fat32, "windows_boot": args.windows_boot, "windows_bios": args.windows_bios, "windows_dual": args.windows_dual, "windows_ntfs": args.windows_ntfs, "windows_version": args.windows_version, "windows_udf": args.windows_udf, "windows_efi_repair": args.windows_efi_repair, "grub_uefi": args.grub_uefi, "grub2_image": args.grub2_image, "grub_ntfs": args.grub_ntfs, "grub2_ext_gui": args.grub2_ext_gui, "grub2_ext_helper": args.grub2_ext_helper, "ext_bits": args.ext_bits, "grub2_helper": args.grub2_helper, "syslinux_uefi": args.syslinux_uefi, "syslinux_image": args.syslinux_image, "syslinux_ntfs": args.syslinux_ntfs, "syslinux_major": args.syslinux_major, "freedos_helper": args.freedos_helper, "freedos_win7": args.freedos_win7, "freedos_boot": args.freedos_boot, "kernel": args.kernel, "accel": args.accel, "ntfs_mounted_helper": args.ntfs_mounted_helper, "fixed_usb_helper": args.fixed_usb_helper, "mounted_raw_helper": args.mounted_raw_helper, "compressed_raw_helper": args.compressed_raw_helper, "compressed_qt_helper": args.compressed_qt_helper, "mounted_raw": args.mounted_raw, "mounted_helper": args.mounted_helper, "mounted_iso_helper": args.mounted_iso_helper, "sfd": args.sfd, "sfd_helper": args.sfd_helper, "handoff": args.handoff, "install": args.install, "helper": args.helper,
                    "uefi_ntfs_helper": args.uefi_ntfs_helper, "uefi_exfat_helper": args.uefi_exfat_helper, "uefi_mbr": args.uefi_mbr,
                    "fat_bits": args.fat_bits, "mbr_fat_helper": args.mbr_fat_helper, "mbr_ntfs_helper": args.mbr_ntfs_helper, "sfd_ntfs_helper": args.sfd_ntfs_helper, "sfd_exfat_helper": args.sfd_exfat_helper, "ntfs_helper": args.ntfs_helper, "fat_helper": args.fat_helper, "exfat": args.exfat, "exfat_helper": args.exfat_helper, "mbr_exfat_helper": args.mbr_exfat_helper,
                    "inputs": {str(path): digest(path) for path in inputs}}
        manifest["xz_raw_helper"] = args.xz_raw_helper
        manifest["bzip2_raw_helper"] = args.bzip2_raw_helper
        manifest["bzip2_qt_helper"] = args.bzip2_qt_helper
        manifest["lzma_raw_helper"] = args.lzma_raw_helper
        manifest["lzw_raw_helper"] = args.lzw_raw_helper
        manifest["zip_raw_helper"] = args.zip_raw_helper
        manifest["zstd_raw_helper"] = args.zstd_raw_helper
        manifest["vtsi_raw_helper"] = args.vtsi_raw_helper
        manifest["vhd_raw_helper"] = args.vhd_raw_helper
        manifest["vhd_qt_helper"] = args.vhd_qt_helper
        manifest["virtual_disk_chains"] = args.virtual_disk_chains
        manifest["vhdx_qt_helper"] = args.vhdx_qt_helper
        manifest["ntfs_options"] = args.ntfs_options
        manifest["iso_options"] = args.iso_options
        manifest["legacy_iso"] = args.legacy_iso
        manifest["geometry_batch"] = args.geometry_batch
        manifest["runtime_validation"] = args.runtime_validation
        manifest["legacy_boot"] = args.legacy_boot
        if args.legacy_boot: manifest["msdos_resource_sha256"] = digest(args.msdos_resource)
        manifest["native_formats"] = args.native_formats
        manifest["udf_indexing_batch"] = args.udf_indexing_batch
        if args.native_formats or args.geometry_batch: manifest["udf_provider_sha256"] = digest(args.mkudffs)
        manifest["full_format"] = args.full_format
        manifest["bad_blocks"] = args.bad_blocks
        manifest["media_appearance"] = args.media_appearance
        manifest["old_bios"] = args.old_bios
        manifest["zip_supplement"] = args.zip_supplement
        manifest["ffu_geometry"] = args.ffu_geometry
        manifest["ffu_layouts"] = args.ffu_layouts
        manifest["ffu_optimized"] = str(args.ffu_optimized) if args.ffu_optimized else None
        manifest["ffu_stores"] = args.ffu_stores
        if args.ffu_catalog:
            manifest["ffu_catalog_sha256"] = digest(args.ffu_catalog)
        manifest["ffu_partial"] = args.ffu_partial
        manifest["image_entry"] = args.image_entry
        manifest["zero_device"] = args.zero_device
        manifest["ffu_variants"] = args.ffu_variants
        manifest["ffu_qt_helper"] = args.ffu_qt_helper
        manifest["save_qt_helper"] = args.save_qt_helper
        manifest["save_export"] = args.save_export
        manifest["save_read_only"] = args.save_read_only
        manifest["vtsi_qt_helper"] = args.vtsi_qt_helper
        manifest["zip_qt_helper"] = args.zip_qt_helper
        manifest["zstd_qt_helper"] = args.zstd_qt_helper
        manifest["lzw_qt_helper"] = args.lzw_qt_helper
        manifest["lzma_qt_helper"] = args.lzma_qt_helper
        manifest["xz_qt_helper"] = args.xz_qt_helper
        manifest["grub4dos_helper"] = args.grub4dos_helper
        manifest["grub4dos_image"] = args.grub4dos_image
        manifest["grub4dos_image_uefi"] = args.grub4dos_image_uefi
        manifest["grub4dos_gui"] = args.grub4dos_gui
        manifest["grub4dos_image_gui"] = args.grub4dos_image_gui
        manifest["grub4dos_ntfs"] = args.grub4dos_ntfs
        manifest["grub4dos_ntfs_driver"] = args.ntfs_mount_driver if args.grub4dos_ntfs else None
        manifest["windows_winpe"] = args.windows_winpe
        manifest["windows_winpe_partial_minint"] = args.windows_winpe_partial_minint
        manifest["windows_winpe_syslinux"] = args.windows_winpe_syslinux
        manifest["windows_winpe_grub_data"] = args.windows_winpe_grub_data
        manifest["windows_winpe_failure"] = args.windows_winpe_failure
        manifest["windows_winpe_gui"] = args.windows_winpe_gui
        manifest["windows_winpe_root_detector"] = args.windows_winpe_root_detector
        manifest["windows_winpe_root_detector_content"] = args.windows_winpe_root_detector_content if args.windows_winpe_root_detector else None
        manifest["windows_winpe_root_sif"] = args.windows_winpe_root_sif
        manifest["windows_winpe_root_loader"] = args.windows_winpe_root_loader
        manifest["windows_winpe_root_loader_content"] = args.windows_winpe_root_loader_content if args.windows_winpe_root_loader else None
        if args.windows_winpe:
            manifest["windows_version"] = None
        (base / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        results = {}
        for sector in ((512,) if (args.handoff or args.install or args.helper) and not (args.exfat or args.sfd) else (512, 4096)):
            size = 1024 * MIB if args.windows_togo else EXT_DISK_BYTES if args.grub2_ext_helper or args.native_formats or args.geometry_batch or args.runtime_validation or args.legacy_boot or args.iso_options or args.ntfs_options or args.iso_resources or args.legacy_iso else 512 * MIB if args.alpine or args.grub2_image else HANDOFF_DISK_BYTES if args.handoff or args.install or args.helper else DISK_BYTES
            if args.alpine:
                disk = base / "usb.raw"
                with disk.open("xb") as target:
                    target.truncate(size)
            else:
                disk = seed_target(base, args.build / "partition_image_probe", size)
            original = digest(disk)
            operation = "Syslinux image helper" if args.syslinux_image else "standalone Syslinux helper" if args.syslinux_major else "FreeDOS helper" if args.freedos_helper else "NTFS helper" if args.ntfs_helper else f"FAT{args.fat_bits} helper" if args.fat_helper else "exFAT helper" if args.exfat_helper else "exFAT handoff" if args.exfat else "helper" if args.helper else "installation" if args.install else "handoff" if args.handoff else "preparation"
            if args.windows_bios:
                operation = "Windows BIOS/dual " + ("FAT32" if args.windows_fat32 else "NTFS") + " helper"
            if args.grub2_image:
                operation = "GRUB ISO NTFS helper" if args.grub_ntfs else f"GRUB ISO FAT{args.fat_bits} helper"
            if args.grub4dos_image:
                filesystem = "NTFS" if args.grub4dos_ntfs else f"FAT{args.fat_bits}"
                operation = f"GRUB4DOS ISO {filesystem} helper"
            if args.grub2_ext_helper:
                operation = f"ext{args.ext_bits} GRUB2 helper"
            layout_name = "SFD" if args.sfd or args.sfd_helper or args.sfd_ntfs_helper or args.sfd_exfat_helper else "MBR"
            print(f"Running {sector}-byte {layout_name} {operation} guest: {base}", flush=True)
            results[str(sector)] = run_guest(args, base, disk, sector)
            if args.geometry_batch:
                from geometry_batch import qualify_boot
                results['firmware_boot'] = qualify_boot(args, base, disk)
            elif args.device_options:
                if args.device_source:
                    expected = json.loads((base / 'mapped-source.json').read_text())
                    assert digest(disk) == expected['sha256'], 'Mapped parent whole-disk restore differs'
                    from device_source import boot
                    results['mapped_restore'] = boot(args, base, disk)
                else: assert digest(disk) == original, 'Unselected USB baseline changed'
                results['readback'] = {'guest_kernel': True, 'device_source': args.device_source, 'classes': ['virtual', 'vmware', 'non-usb-removable'] if not args.device_source else ['loop', 'nbd', 'optical'], 'jobs': 3 if not args.device_source else 9}
            elif args.iso_resources:
                boot_syslinux_uefi(args,base/'resources-uefi',disk)
                from iso_resources import boot_bios
                results['firmware_boot'] = boot_bios(args,base,disk)
            elif args.legacy_iso:
                from legacy_iso import qualify_boot
                results['firmware_boot'] = qualify_boot(args,base,disk)
            elif args.ntfs_options:
                boot_syslinux_uefi(args, base / 'ntfs-options-uefi', disk)
                results['firmware_boot'] = {'native_fixture': True, 'backing_media_unchanged': True, 'compression_boot_exceptions': True}
            elif args.iso_options:
                boot_syslinux_uefi(args, base / 'iso-options-uefi', disk)
                results['firmware_boot'] = {'native_fixture': True, 'backing_media_unchanged': True, 'ntfs_udf_iso_options': True}
            elif args.legacy_boot:
                from legacy_boot import qualify_boot
                results['firmware_boot'] = qualify_boot(args,base,disk)
            elif args.runtime_validation:
                boot_syslinux_uefi(args, base / 'runtime-uefi', disk)
                results['firmware_boot'] = {'native_fixture': True, 'validator_chainload': True, 'backing_media_unchanged': True}
                from runtime_validation import qualify_corruption
                results['corruption_control'] = qualify_corruption(args, base, disk)
            elif args.native_formats:
                results["readback"] = {"guest_kernel": True,
                    "formats": ["udf"] if args.udf_indexing_batch else ["udf", "ext2", "ext3"],
                    "jobs": 2 if args.udf_indexing_batch else 4, "non_bootable": True}
            elif args.windows_togo:
                boot_syslinux_uefi(args, base / 'togo-uefi', disk)
                results['firmware_boot'] = {'native_fixture': True, 'windows_execution': False, 'backing_media_unchanged': True}
            elif args.freedos_helper:
                results["readback"] = inspect_freedos(args, base, disk)
                if args.freedos_boot:
                    from freedos_boot import qualify_boot
                    results["bios_boot"] = qualify_boot(args, base, disk)
            elif args.sfd or args.sfd_helper:
                if sector == 512:
                    from test_sfd_prepare import inspect_fat
                    with disk.open("rb") as stream:
                        boot = stream.read(512)
                    short_sectors = struct.unpack_from("<H", boot, 19)[0] if args.fat_bits == 16 else 0
                    sectors = short_sectors or struct.unpack_from("<I", boot, 32)[0]
                    report = {"BYTES": sectors * 512}
                    tools = argparse.Namespace(fsck="/usr/bin/fsck.fat", mdir="/usr/bin/mdir", sfdisk="/usr/bin/sfdisk")
                    inspect_fat(tools, disk, size, report, args.fat_bits, FAT16_CLUSTER_BYTES if args.fat_bits == 16 else SECTOR_BYTES)
                    results["readback"] = {"offset": 0, "capacity": size, "filesystem_bytes": report["BYTES"], "sha256": digest(disk)}
                else:
                    assert digest(disk) == original, "Rejected 4K SFD target changed"
            elif args.alpine:
                from syslinux_alpine import qualify_disk
                results["readback"] = qualify_disk(args, base, disk)
            elif args.syslinux_ntfs:
                results["readback"] = inspect_syslinux_ntfs(args, base, disk)
            elif args.install or args.helper or args.syslinux_major:
                results["readback"] = inspect_install(args, base, disk)
            elif args.exfat or args.exfat_helper:
                if sector == 512:
                    results["readback"] = inspect_exfat(args, base, disk)
                else:
                    assert digest(disk) == original, "Rejected 4K exFAT target changed"
            elif args.grub2_ext_helper:
                results["readback"] = inspect_grub2_ext(args, base, disk)
            elif args.grub2_ntfs_helper:
                results["readback"] = inspect_grub2_ntfs(args, base, disk)
            elif args.ntfs_helper and not (args.grub4dos_helper or args.grub4dos_image):
                results["readback"] = inspect_ntfs_format(args, base, disk)
            elif args.windows_bios:
                if args.windows_winpe:
                    from winpe_helper import inspect_written
                else:
                    from windows_bios import inspect_written
                results["readback"] = inspect_written(args, base, disk)
                if args.windows_boot or args.windows_efi_repair:
                    from windows_bios import qualify_boot
                    results["firmware_boot"] = qualify_boot(args, base, disk)
                    results["readback"]["bios_boot_qualified"] = not args.windows_efi_repair
                    results["readback"]["efi_boot_qualified"] = args.windows_dual
            elif args.grub2_image:
                from grub2_iso import inspect_and_boot
                results["readback"] = inspect_and_boot(args, base, disk)
            elif args.grub4dos_helper or args.grub4dos_image:
                results["readback"] = inspect_grub4dos(args, base, disk)
                from grub4dos_boot import qualify_boot, check_missing_loader
                loader = prepare.HERE.parent / "fixtures/download/grub4dos-0.4.6a/grldr"
                results["bios_boot"] = qualify_boot(args, base, disk, loader)
                results["boot_control"] = check_missing_loader(args, base, disk, loader,
                    results["readback"]["offset"], results["readback"]["size"])
                results["readback"]["boot_execution_qualified"] = True
                if args.grub4dos_image_uefi:
                    boot_syslinux_uefi(args, base / "grub4dos-uefi", disk, native_fat=not args.grub4dos_ntfs)
                    filesystem = "NTFS" if args.grub4dos_ntfs else f"FAT{args.fat_bits}"
                    results["uefi_boot"] = {"firmware": "uefi", "filesystem": filesystem,
                        "native_fat": not args.grub4dos_ntfs, "passed": True, "backing_media_unchanged": True,
                        "efi_fixture_sha256": digest(args.build / "uefi-handoff.efi")}
                    results["readback"]["efi_boot_qualified"] = True
            elif args.grub2_helper:
                results["readback"] = inspect_grub2(args, base, disk)
            elif args.fat_helper:
                results["readback"] = inspect_fat_format(args, base, disk)
            elif args.handoff:
                results["readback"] = inspect_handoff(base, disk, args.fat_bits)
            elif sector == 512:
                inspect_cleared(disk)
            else:
                assert digest(disk) == original, "Rejected 4K device changed"
            disk.unlink()
        for path in inputs:
            assert digest(path) == manifest["inputs"][str(path)], path
        (base / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        if args.geometry_batch:
            print('PASS: B5 geometry batch: 21 ordinary Qt jobs, seven native formatters, MBR/GPT/SFD, actual 512/512e/4Kn block devices, independent metadata/payload/kernel readback, joined READY and representative FAT32 EFI boot.', flush=True)
        elif args.device_options:
            print('PASS: grouped B2 loop/NBD/optical/reset/ESP/backing route.' if args.device_source else 'PASS: grouped virtual/VMware/removable Qt creation, kernel readback, capture geometry and home protection; unselected USB unchanged.', flush=True)
        elif args.legacy_iso:
            print('PASS: B4 Windows/WinPE FAT16, ReactOS ISO and Kolibri ISO: five ordinary Qt jobs, original records, NTFS dual layout/clean volume, independent byte/attribute readback, joined READY and distinct EFI/multiboot/Kolibri handoffs.', flush=True)
        elif args.ntfs_options:
            print("PASS: NTFS compression/indexing batch: three ordinary Qt jobs, native flags/data readback, boot exceptions and EFI boot.", flush=True)
        elif args.iso_options:
            print("PASS: ISO options batch: FAT16 DOS 64KiB override, FAT32 ISO and NTFS UDF ordinary Qt jobs, timestamps, bytes, joined READY and native EFI boot.", flush=True)
        elif args.legacy_boot:
            print("PASS: combined MS-DOS/ReactOS FAT16/FAT32 ordinary Qt creation, independent payload/attribute readback and bare ReactOS BIOS handoff.",flush=True)
        elif args.runtime_validation:
            print("PASS: runtime validation batch: three ordinary Qt jobs, independent checksum/resource/original-loader readback, backup prewrite refusal and native EFI chainload.", flush=True)
        elif args.native_formats:
            if args.udf_indexing_batch:
                print("PASS: UDF indexing batch: two ordinary Qt jobs, saved indexing choices, optional diagnostic, exact label/icon/ZIP readback, one authorization and joined READY; previous writer/boot evidence reused.", flush=True)
            else:
                print("PASS: native UDF/ext data batch: four ordinary Qt jobs, one authorization each, joined READY, independent kernel readback; previous writer/boot evidence reused.", flush=True)
        elif args.windows_togo:
            print('PASS: WIM/ESD and both ISO forms, GPT/MBR, ordinary Qt, native readback and EFI handoff.', flush=True)
        elif args.syslinux_ntfs:
            mode = 'image' if args.syslinux_image else 'standalone'
            print(f"PASS: {len(results['512'])} {mode} NTFS helper jobs ({args.syslinux_ntfs}), one authorization, cleanup, readback and BIOS boot.", flush=True)
        elif args.syslinux_major:
            print(f"PASS: eleven FAT{args.fat_bits} standalone Syslinux helper jobs, single launch, two copy events, cleanup, independent readback and bare BIOS boot.", flush=True)
        elif args.freedos_helper:
            print(f"PASS: six FAT{args.fat_bits} FreeDOS helper jobs, discrete progress, single launch, cleanup and independent readback.", flush=True)
        elif args.sfd:
            print(f"PASS: 13 whole-device FAT{args.fat_bits} handoffs, 4K refusal, stale partition removal and independent SFD readback.", flush=True)
        elif args.alpine:
            print("PASS: complete Alpine Syslinux helper job, independent readback and BIOS/UEFI USB boots.", flush=True)
        elif args.zip_supplement:
            print("PASS: ZIP supplement batch: CRC and source-on-target prewrite refusals, ordinary Qt formatting, bootable ISO overlay, readback and BIOS boot.", flush=True)
        elif args.helper:
            if args.save_read_only:
                print("PASS: read-only discovery/capture batch: VHD/VHDX/FFU/ISO, kernel write-protection unchanged, write refusal, UDF readback and one VHDX restore/boot.", flush=True)
            elif args.save_export:
                print("PASS: ISO export/source unmount batch: six single-authorization Qt jobs, FAT/NTFS kernel UDF readback, cleanup/cancellation, VHDX restore and BIOS boot.", flush=True)
            elif args.save_qt_helper:
                print("PASS: device-saving batch: unprivileged VHD/VHDX/FFU capture, single read authorization, cancellation/denial, joined READY, full VHDX restore, readback and BIOS boot.", flush=True)
            elif args.zero_device:
                print("PASS: device zeroing batch: fast mixed/empty and full zero jobs, single-helper joined READY, ordinary restore/readback and BIOS boot.", flush=True)
            elif args.image_entry:
                print("PASS: image-entry batch: default marker refusals, explicit raw/compressed overrides, normal restore, one helper per job, readback and BIOS boot.", flush=True)
            elif args.ffu_partial:
                print("PASS: partial FFU batch: full restore, wrong-base prewrite refusal, sparse update, single-helper joined READY, independent disk/FAT readback and BIOS boot.", flush=True)
            elif args.old_bios:
                print("PASS: old-BIOS batch: five native filesystems, complete helpers, ordinary Qt choice/READY, independent GRUB BIOS boot and one raw restore/readback/boot.", flush=True)
            elif args.media_appearance:
                print("PASS: Unicode labels/icons across FAT16/FAT32/NTFS/exFAT and SFD, kernel/FUSE readback, preserved image autorun, default Qt job and one restore/BIOS boot.", flush=True)
            elif args.bad_blocks:
                print("PASS: device patterns, direct-I/O fake detection, Abort/Retry/Ignore, one authorization, joined READY and ordinary restore/BIOS boot.", flush=True)
            elif args.full_format:
                print("PASS: seven full formatters, native readback, one authorization/joined helper jobs and one representative Qt restore/BIOS boot.", flush=True)
            elif args.ffu_geometry:
                print("PASS: FFU geometry: exact-sector read-only device capture, ordinary Qt restore/GPT relocation, one full restore/readback/BIOS boot.", flush=True)
            elif args.ffu_variants:
                print("PASS: FFU variants: LZNT1/Huffman grouped buffers, end-relative runs, single-helper joined READY, independent disk/FAT readback and one bootable restore.", flush=True)
            elif args.ffu_optimized:
                print("PASS: optimized FFU smaller/larger target jobs, NTFS growth, moved recovery/XML readback, joined READY and one representative full restore/BIOS boot.", flush=True)
            elif args.ffu_stores:
                print("PASS: V2 FFU stores: two explicit store jobs and one full store-2 restore, single-helper joined READY, independent readback and BIOS boot.", flush=True)
            elif args.ffu_qt_helper:
                print("PASS: FFU batch: one seed and two ordinary XPRESS/full Qt jobs, single-helper joined READY, independent disk/FAT-file readback and BIOS boot; shared fault matrix reused.", flush=True)
            elif args.virtual_disk_chains:
                print("PASS: parent/log batch: one seed, VHD/VHDX child jobs, parent protections, one parent-plus-log bootable restore, joined READY, independent readback and BIOS boot.", flush=True)
            elif args.vhdx_qt_helper:
                print("PASS: VHDX batch: one seed and two ordinary dynamic/fixed Qt jobs, single-helper joined READY, independent disk/FAT-file readback and BIOS boot; shared fault matrix reused.", flush=True)
            else:
                print("PASS: five production helper jobs, refusal before writes, installer cancellation, readback and MBR boot.", flush=True)
            if args.fixed_usb_helper:
                print("PASS: fixed-USB protections with empty UDisks mounts, single-launch mounted DD and unmounted format/DD, readback and boot.", flush=True)
            if args.mounted_raw_helper and not (args.vhdx_qt_helper or args.ffu_qt_helper or args.save_qt_helper):
                print("PASS: five mounted/unmounted DD client/helper jobs, one launch each, source/busy refusal, EOF cancellation, readback and boot.", flush=True)
            if args.xz_qt_helper:
                print("PASS: ordinary gzip and XZ SELECT/START/controller, two full production-helper restores plus 45 direct jobs, joined exit/readback and BIOS boot.", flush=True)
            if args.compressed_qt_helper:
                print("PASS: ordinary gzip SELECT/START/controller, native geometry, one helper, joined completion and independent BIOS readback/boot.", flush=True)
            if args.compressed_raw_helper:
                print("PASS: 15 gzip helper cases on a real guest block device, claim/write/cancel/sync/readback/close gates, complete compressed restore and independent BIOS boot.", flush=True)
            if args.xz_raw_helper:
                print("PASS: 20 XZ helper cases plus 25 seed/raw/gzip regressions, real guest claims/write/cancel/sync/readback/close, complete XZ restore and independent BIOS boot.", flush=True)
            if args.bzip2_qt_helper:
                print("PASS: ordinary BZIP2 SELECT/START/confirmation/controller, 64-MiB production-helper restore, joined exit/readback and BIOS boot; five seed and five raw setup jobs retained.", flush=True)
            if args.lzma_qt_helper:
                print("PASS: ordinary LZMA SELECT/START/confirmation/controller, 64-MiB production-helper restore, joined exit/readback and BIOS boot; five seed and five raw setup jobs retained.", flush=True)
            if args.bzip2_raw_helper:
                print("PASS: 22 BZIP2 helper cases plus 47 gzip/XZ regressions, real guest claims/CRC/mount/write/cancel/sync/readback/close, poisoned full restore and independent BIOS boot.", flush=True)
            if args.lzma_raw_helper:
                print("PASS: 38 focused LZMA jobs: five seed/five raw, four mounted and 24 direct source/size/history/dictionary/claim/fault cases, poisoned 64-MiB production-helper restore/readback and independent BIOS boot.", flush=True)
            if args.lzw_raw_helper:
                print("PASS: focused LZW real-kernel format/source/claim/data faults, four mounted jobs, poisoned 64-MiB production-helper restore/readback and independent BIOS boot; five seed and five raw setup jobs retained.", flush=True)
            if args.zip_raw_helper:
                print("PASS: five ZIP methods on a real guest block device, source/CRC/claim/fault/mounted jobs, poisoned 64-MiB production-helper deflate restore/readback and independent BIOS boot; five seed and five raw setup jobs retained.", flush=True)
            if args.zstd_raw_helper:
                print("PASS: Zstd real guest source/checksum/claim/fault jobs, poisoned 64-MiB production-helper restore/readback and independent BIOS boot; five seed and five raw setup jobs retained.", flush=True)
            if args.vtsi_raw_helper and not args.vtsi_qt_helper:
                print("PASS: VTSI real guest sparse gap preservation, kernel claim and readback, poisoned 64-MiB production-helper restore and independent BIOS boot; five seed and five raw setup jobs retained.", flush=True)
            if args.vhd_raw_helper and not args.vhd_qt_helper:
                print("PASS: fixed/dynamic VHD real guest, stale-hash refusal, kernel claim/readback, poisoned 64-MiB production-helper restore and independent BIOS boot.", flush=True)
            if args.vhd_qt_helper:
                print("PASS: ordinary fixed/dynamic VHD SELECT/confirmation/joined READY, two single-helper restores, logical/tail readback and independent BIOS boot.", flush=True)
            if args.vtsi_qt_helper:
                print("PASS: ordinary VTSI sparse SELECT/confirmation/joined READY, two single-helper restores and independent gap/readback/BIOS checks.", flush=True)
            if args.zip_qt_helper:
                print("PASS: ordinary ZIP SELECT/START/confirmation/controller, 64-MiB production-helper restore, single launch, held 99 percent, joined READY/readback and BIOS boot; five seed and five raw setup jobs retained.", flush=True)
            if args.zstd_qt_helper:
                print("PASS: ordinary Zstd SELECT/START/confirmation/controller, 64-MiB production-helper restore, single launch, held 99 percent, joined READY/readback and BIOS boot; five seed and five raw setup jobs retained.", flush=True)
            if args.lzw_qt_helper:
                print("PASS: ordinary LZW and gzip fallback SELECT/controller, two full64MiB production-helper restores, five real fallback source/data faults, joined READY/readback and BIOS boot; five seed/five raw setup jobs retained.", flush=True)
            if args.ntfs_mounted_helper:
                print("PASS: 14 NTFS-3G mounted helper jobs, both drivers, refusal/cancellation, one launch, FAT32 format and full DD readback before boot.", flush=True)
            if args.mounted_raw:
                print("PASS: 14 native mounted DD cases, refusal/cancellation/flush/readback failures, whole-disk equality and boot.", flush=True)
            if args.mounted_iso_helper:
                print("PASS: three mounted-partition ISO jobs, source-change refusal, cancellation and single-authorization creation before readback and boot.", flush=True)
        elif args.install:
            print("PASS: eleven native block installer cases, retained exclusive claim, independent readback and MBR boot.", flush=True)
        elif args.standalone_uefi:
            filesystem = "NTFS" if args.ntfs_helper else "exFAT"
            table = "MBR" if args.uefi_mbr else "GPT"
            print(f"PASS: {len(results['512']) - 4} non-bootable {filesystem} regressions and four standalone {table}/UEFI:NTFS jobs, final sync/cancel/exit ordering, payload and filesystem readback.", flush=True)
        elif args.grub2_ext_gui:
            print(f"PASS: Qt Widgets MBR/ext{args.ext_bits} creation, one authorization, final READY, native readback and bare BIOS boot.", flush=True)
        elif args.grub2_ext_helper:
            print(f"PASS: MBR/ext{args.ext_bits} GRUB2 helper, one authorization, native filesystem readback and bare BIOS boot.", flush=True)
        elif args.grub2_ntfs_helper:
            print("PASS: MBR/NTFS GRUB2 helper job, one authorization, original records, NTFS readback and bare BIOS boot.", flush=True)
        elif args.ntfs_helper and not (args.grub4dos_helper or args.grub4dos_image):
            print(f"PASS: {len(results['512']) - 1} non-bootable NTFS jobs and one bootable regression, retained claims, sync and independent readback.", flush=True)
        elif args.windows_bios:
            filesystem = "FAT32" if args.windows_fat32 else "NTFS"
            if args.windows_winpe_failure:
                print("PASS: failed legacy root write, FAILED after helper exit, clean unmount/released claim, source-tree readback and no final masquerading record.", flush=True)
            elif args.windows_winpe:
                print(f"PASS: complete legacy {filesystem} helper job, one simulated authorization, prepared-file readback, sync/cleanup/exit and generated BIOS handoff.", flush=True)
            else:
                print(f"PASS: native Windows BIOS/dual {filesystem} creation, one simulated authorization, source retention, sync/cleanup/exit and exact independent readback.", flush=True)
            if args.windows_boot:
                print("PASS: generated BOOTMGR BIOS handoff, original Rufus MBR prompt choices when selected, and optional x64 EFI handoff. Windows Setup remains unqualified.", flush=True)
            if args.windows_efi_repair:
                print("PASS: exact fallback installed from first install image, unchanged root manager, and x64 EFI execution. Windows Setup remains unqualified.", flush=True)
        elif args.grub2_image:
            print(f"PASS: GRUB ISO {'NTFS' if args.grub_ntfs else f'FAT{args.fat_bits}'} helper, one launch, source/core snapshots, copy/finalization, independent readback and BIOS Linux boot.", flush=True)
            if args.grub_uefi:
                print("PASS: complete bundled UEFI:NTFS payload and x64 EFI application handoff from NTFS; snapshot media unchanged.", flush=True)
        elif args.grub4dos_image:
            filesystem = "NTFS" if args.grub4dos_ntfs else f"FAT{args.fat_bits}"
            print(f"PASS: native GRUB4DOS ISO {filesystem} creation, one simulated authorization, ISO progress, sync/cleanup/held exit, exact image files/records and original loader/menu execution. Missing-loader control rejected.", flush=True)
            if args.grub4dos_image_uefi:
                route = "through the original UEFI:NTFS payload" if args.grub4dos_ntfs else f"directly from FAT{args.fat_bits}"
                print(f"PASS: image-owned x64 EFI fallback executes {route}; unchanged snapshot backing media.", flush=True)
        elif args.grub4dos_gui:
            filesystem = "NTFS" if args.grub4dos_ntfs else f"FAT{args.fat_bits}"
            print(f"PASS: Qt GRUB4DOS {filesystem} creation, one simulated authorization, native sync/cleanup/held exit, original loader readback and BIOS execution. Missing-loader control rejected.", flush=True)
        elif args.grub4dos_helper:
            filesystem = "NTFS" if args.grub4dos_ntfs else f"FAT{args.fat_bits}"
            print(f"PASS: native GRUB4DOS {filesystem} helper job, one simulated authorization, retained loader, sync/cleanup/held exit, original records, independent grldr readback and actual BIOS loader execution. Missing-loader control rejected.", flush=True)
        elif args.grub2_helper:
            print(f"PASS: {len(results['512'])} FAT{args.fat_bits} GRUB2 helper jobs, one authorization each, no copy events, cancellation, sync, readback and bare BIOS boot.", flush=True)
        elif args.fat_helper:
            print(f"PASS: {len(results['512'])} FAT{args.fat_bits} client/helper jobs, preflight refusals, cancellation, final sync and independent readback.", flush=True)
            if args.mounted_helper:
                print("PASS: 12 mounted-target helper jobs, one authorization each, unchanged disks on failure/cancellation and clean creation.", flush=True)
        elif args.exfat_helper:
            print(f"PASS: {len(results['512'])} exFAT client/helper jobs, preflight refusals, cancellation, final sync and independent readback.", flush=True)
        elif args.exfat:
            print("PASS: 19 exFAT block/claim cases, native partition-offset metadata, independent readback and sync.", flush=True)
        elif args.handoff:
            print("PASS: 13 handoff cases, full-volume readback, sync/unmount and retained claims.", flush=True)
        else:
            print("PASS: eight block-device requests, GPT replacement, retained claim and kernel MBR layout.", flush=True)
    finally:
        # Keep evidence, not disposable guest/media data, including on failure.
        for name in ("root", "initramfs", "fixture", "expected", "unknown", "incompatible", "conflict", "efi", "bios", "alpine-source-tree", "RUFUS_SOURCE", "iso-expected", "grub-source", "grub-boot-fixture", "grub-readback", "windows-source", "windows-image-readback", "grub4dos-source"):
            if (base / name).exists():
                shutil.rmtree(base / name)
        for name in ("vmlinuz", "initramfs.cpio", "usb.raw", "volume.raw", "filled.bin", "readback", "check.img"):
            (base / name).unlink(missing_ok=True)
        if args.legacy_iso:
            for directory in base.glob('legacy-source-*'):
                shutil.rmtree(directory)
            for name in ('legacy-windows.raw','legacy-reactos.raw','multiboot.bin','kolibri-loader.bin'):
                (base/name).unlink(missing_ok=True)
        if args.geometry_batch:
            for path in base.glob('geometry-*.raw'):
                path.unlink()
        if args.windows_bios:
            (base / "windows-source.iso").unlink(missing_ok=True)
            for directory in base.glob("windows-bios-*"):
                (directory / "second.raw").unlink(missing_ok=True)
                (directory / "vga.bin").unlink(missing_ok=True)
        if args.grub2_image:
            (base / "grub-source.iso").unlink(missing_ok=True)
        if args.grub4dos_image:
            (base / "grub4dos-source.iso").unlink(missing_ok=True)
        if args.syslinux_image:
            for name in ('RUFUS_SOURCE.iso', 'unknown.iso', 'incompatible.iso', 'conflict.iso', 'efi.iso', 'bios.iso', 'fixture.bs'):
                (base / name).unlink(missing_ok=True)
        for path in base.glob("*-bios/*.bin"):
            path.unlink()
        for path in base.glob("oracle*\\*"):
            path.unlink()


if __name__ == "__main__":
    main()
