# Native USB guest qualification

This optional test runs the actual Qt Widgets, UDisks/polkit acquisition and C
writer inside a disposable Linux guest. It uses real guest block devices without
linker wrappers. The normal CTest suite never runs it on the host.

The current offline recipe uses installed Arch/CachyOS package metadata and
matching archives from `/var/cache/pacman/pkg`. Build the executable on the same
system so its Qt and C++ runtime match the guest packages. It needs Python 3.11+,
bsdtar, kmod, cpio, OpenSSL, mkfs.ext4, mkfs.fat, NASM, QEMU x86, debugfs, and
SeaBIOS at `/usr/share/qemu/bios-256k.bin`. The
installed guest dependencies include UDisks2, polkit, the KDE authentication
agent, Qt6, Xvfb, xdotool and DejaVu fonts. Missing cached dependencies are errors;
the recipe never downloads packages or installs anything on the host.

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_BUILD_VM_TEST=ON
cmake --build /tmp/rufus-linux-qt6-build
python3 linux/tests/vm/prepare.py --build /tmp/rufus-linux-qt6-build \
    --kernel 6.18.50-3-cachyos-lts /tmp/rufus-usb-qualification
python3 linux/tests/vm/run.py /tmp/rufus-usb-qualification /tmp/rufus-usb-run
```

Use an installed kernel release with its matching modules. Both output paths
must be new directories. The recipe records package versions and SHA-256 hashes,
the kernel release, and hashes of the executable, kernel, initramfs and disk
images in `manifest.json`. The runner checks the prepared images before use and
records its QEMU command. Allow roughly 7 GiB for preparation and one run; the
unpacked package tree can be removed after preparation. KVM requires access to
`/dev/kvm`; `--accel tcg` selects software emulation, subject to the same timeout.
Only KVM is qualified so far.

Each run attaches two newly created regular-file disks: the guest system and a
256 MiB emulated removable USB disk with serial `RUFUS-TEST-USB`. There is no
network, host filesystem sharing, host block-device attachment or USB passthrough.
Guest root setup assigns ownership and starts the services. The test itself runs
as UID 1000, verifies that direct raw writes are denied, and refuses to start
unless the fixture marker, QEMU DMI product and kernel command line all match.
It also checks the selected USB serial, path and capacity before starting.

The guest has fresh accounts and a public disposable password, `rufus-fixture`.
Its polkit rule only chooses the fixture administrator; normal UDisks policy
still requires authentication. xdotool enters the password into the separate
native KDE agent. Rufus neither receives the password nor runs as root. Xvfb
provides a real X11 surface for the Widgets and native prompt; this is not a
complete desktop session or a claim of visual parity with Windows. The native
agent uses its software renderer under Xvfb; the Rufus frontend uses Widgets.

The tests dismiss authentication, authenticate successfully, write a generated
17 MiB BIOS/FAT16 image, flush and verify it, and wait for native partition
discovery. QEMU throttles writes to 2 MiB/s for the following fault cases: a
competing guest mount must report busy while the writer owns the disk, then
succeed after cancellation releases it; unplugging through QMP during a write
must end as failure and clear discovery without reporting 100%. Reattaching the
same USB at the same device and UDisks paths must produce a new identity. The old
selection and retained write plan must remain rejected, without authentication
or writing; the user must select the newly discovered device.

The host independently compares every written byte and the untouched target
tail. It saves the successfully verified output before the cancellation/removal
cases, then boots that saved disk read-only in a second SeaBIOS VM. The original
`boot.asm` fixture prints `RUFUS_RAW_BOOT_OK` to COM1 and exits QEMU with status 33.
This proves that the generated fixture reaches its BIOS entry point. It does not
qualify an OS installer, physical USB durability or Windows.

Results include `serial.log`, `qemu.log`, `authentication.png`, `complete.png`,
`mount-error`, `verified-usb.raw`, `boot.log`, the retained `bios.bin`, its hash
and QEMU version in `firmware.json`, and the manifest/command records.
A missing assertion, timeout, changed input hash or missing boot marker fails
the run. No VM remains running after runner failure.

## Optional real Linux images

The extended run writes the official OpenWrt 25.12.5 x86/64 ext4 combined BIOS
and UEFI images through the same Widgets workflow. These are bootable raw disk
images. Download and decompress them during fixture preparation; this does not
add compressed-image support to the application.

```sh
python3 linux/tests/vm/openwrt.py /tmp/rufus-openwrt-downloads
python3 linux/tests/vm/prepare.py --build /tmp/rufus-linux-qt6-build \
    --kernel 6.18.50-3-cachyos-lts --openwrt /tmp/rufus-openwrt-downloads \
    /tmp/rufus-openwrt-guest
python3 linux/tests/vm/run.py /tmp/rufus-openwrt-guest /tmp/rufus-openwrt-run
```

`openwrt.py` downloads two pinned archives over HTTPS and checks their published
SHA-256 hashes. Preparation verifies them again before decompression and records
their URLs, release, compressed hashes and raw hashes in the manifest. Existing
archives allow offline preparation. The sources and checksums are published in
the [official release directory](https://downloads.openwrt.org/releases/25.12.5/targets/x86/64/).
No signature verification is claimed. Allow roughly 10 GiB for this extended
preparation and run. In addition to the baseline dependencies, the download
needs curl and UEFI boot needs `/usr/share/edk2/x64/OVMF_CODE.4m.fd` and
`OVMF_VARS.4m.fd`.

After each verified write, the guest observer saves the closed device before the
next write can begin. The host compares the complete written range against the
source and checks that the tail is unchanged. The UEFI source is slightly
shorter, so its tail is compared against the preceding BIOS result. The official
image bytes, including partition tables, are preserved without repairs.

Each saved output boots as emulated USB in a separate VM with SeaBIOS or OVMF.
The test enters its serial shell, reads `/etc/openwrt_release`, checks `uname -m`
and `/sys/firmware/efi`, and requires an exact output marker for the expected
release, architecture and boot mode before powering off. QEMU uses a temporary
snapshot for first-boot writes; the saved disk hash must remain unchanged.
Firmware copies, hashes, QEMU commands and serial logs accompany both disk
snapshots. This establishes userspace boot for these two images and firmware
configurations. Secure Boot, other distributions and OS installation remain
separate qualification work.
