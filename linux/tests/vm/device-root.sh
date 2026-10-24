#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
[[ $(< /sys/class/dmi/id/product_name) == RufusUSBFixture ]]
[[ $(< /etc/rufus-gpt-fixture) == RUFUS-GPT-TEST ]]
trap 'echo "RUFUS_NATIVE_SUPERVISOR_FAILED $LINENO"; systemctl poweroff' ERR
chmod 4755 /usr/bin/pkexec
chown -R 1000:1000 /home/rufus
if [[ -f /etc/rufus-extract-fixture ]]; then
    chown 0:0 /opt/rufus/root-owned.iso
fi
mkdir /target
modprobe -a xhci_pci usb_storage sd_mod vfat nls_cp437 nls_iso8859_1
if [[ -f /etc/rufus-wim-split-fixture ]]; then
    modprobe -a 9p 9pnet_virtio
    mkdir /source
    mount -t 9p -o trans=virtio,version=9p2000.L,ro source /source
fi
if [[ -f /etc/rufus-ntfs-fixture ]]; then
    modprobe -a ntfs3 nls_utf8
    if [[ -f /etc/rufus-ntfs-mount ]]; then
        modprobe fuse
        [[ $(< /etc/rufus-ntfs-mount) != ntfs ]] || modprobe ntfs
    fi
fi
systemctl start polkit.service udisks2.service
udevadm wait --timeout=10 /dev/sda
# BLAKE2 keeps whole-disk checks practical under CPU emulation for this fixture.
checksum=sha256sum
[[ ! -f /etc/rufus-large-file-path ]] || checksum=b2sum
[[ ! -f /etc/rufus-wim-split-fixture ]] || checksum=b2sum
"$checksum" /dev/sda > /run/rufus-original-disk.checksum
stdbuf -oL dbus-monitor --system \
    "type='method_call',interface='org.freedesktop.PolicyKit1.Authority',member='CheckAuthorization'" \
    > /home/rufus/authorization.log 2>/home/rufus/authorization-errors.log &
monitor=$!
sleep .2
systemctl start --no-block rufus-gui.service
attempts=4200
[[ ! -f /etc/rufus-large-file-path ]] || attempts=11000
[[ ! -f /etc/rufus-wim-split-fixture ]] || attempts=36000
for ((attempt=0; attempt<attempts; attempt++)); do
    # Retain enough state to diagnose a stalled desktop authorization in TCG.
    if ((attempt % 300 == 0)); then
        ps -eo pid,ppid,stat,comm | grep -E 'rufus|test_prepare|polkit|Xvfb|xdotool|pkexec' || true
        runuser -u rufus -- env DISPLAY=:0 xdotool search --onlyvisible --name '.*' getwindowname %@ || true
    fi
    extractor=''
    if [[ -f /etc/rufus-large-file-path || -f /etc/rufus-windows-fixture || -f /etc/rufus-wim-split-fixture ]]; then
        # Observe the production helper directly while it streams the large file.
        candidate=$(pgrep -f '^/opt/rufus/rufus-prepare-helper --device$' || true)
        if [[ -n $candidate ]] && grep -q ' /run/rufus-' "/proc/$candidate/mountinfo"; then
            extractor=$candidate
        fi
    elif [[ -f /run/rufus-extractor.pid ]]; then
        extractor=$(< /run/rufus-extractor.pid)
    fi
    if [[ -n $extractor ]]; then
        checked=''
        [[ ! -f /home/rufus/private-mount-ok ]] || checked=$(< /home/rufus/private-mount-ok)
        if [[ -n $extractor && $extractor != "$checked" ]]; then
            grep ' /run/rufus-' "/proc/$extractor/mountinfo" > /home/rufus/private-mount-info
            cat /home/rufus/private-mount-info
            read -r mount_id parent_id device_id mount_root mount_point mount_options mount_rest \
                < /home/rufus/private-mount-info
            for flag in rw nosuid nodev noexec; do
                [[ ,$mount_options, == *,"$flag",* ]]
            done
            ! grep -q ' /run/rufus-' /proc/self/mountinfo
            if [[ -f /etc/rufus-ntfs-mount ]]; then
                driver=$(< /etc/rufus-ntfs-mount)
                [[ " $mount_rest " == *" - $driver "* ]]
                echo "RUFUS_NTFS_MOUNT $driver"
            fi
            echo "$extractor" > /home/rufus/private-mount-ok
            echo RUFUS_NATIVE_PRIVATE_MOUNT
        fi
    fi
    if [[ -f /home/rufus/check-unchanged && ! -f /home/rufus/unchanged-ok ]]; then
        "$checksum" -c /run/rufus-original-disk.checksum
        touch /home/rufus/unchanged-ok
        echo RUFUS_NATIVE_UNCHANGED
    fi
    if [[ -f /home/rufus/mount-target && ! -f /home/rufus/mounted-ready ]]; then
        "$checksum" /dev/sda > /run/rufus-prepared-disk.checksum
        if [[ -f /etc/rufus-ntfs-fixture ]]; then
            mount -t ntfs3 -o ro /dev/sda1 /target
            start=$(< /sys/class/block/sda1/start)
            sectors=$(< /sys/class/block/sda1/size)
            /opt/rufus/ntfs_format_probe /usr/bin/mkntfs /dev/sda1 \
                "$((start * 512))" "$((sectors * 512))" 4096 'Rufus NTFS ș😀' native \
                > /home/rufus/mounted-formatter.log 2>&1
            cat /home/rufus/mounted-formatter.log
            grep -q 'ERROR=5 STARTED=1' /home/rufus/mounted-formatter.log
            "$checksum" -c /run/rufus-prepared-disk.checksum
            echo RUFUS_NTFS_FORMATTER_MOUNTED_REFUSED
        else
            mount -t vfat -o ro /dev/sda1 /target
        fi
        touch /home/rufus/mounted-ready
    fi
    if [[ -f /home/rufus/unmount-target && ! -f /home/rufus/mounted-refusal-ok ]]; then
        umount /target
        "$checksum" -c /run/rufus-prepared-disk.checksum
        touch /home/rufus/mounted-refusal-ok
        echo RUFUS_NATIVE_MOUNTED_REFUSAL
    fi
    [[ -f /home/rufus/test-exit ]] && break
    sleep .1
done
systemctl stop rufus-gui.service || true
if [[ -f /etc/rufus-wim-split-fixture ]]; then
    [[ $(< /home/rufus/test-exit) == 0 ]]
    ! pgrep -f '^/opt/rufus/rufus-prepare-helper --device$'
    ! grep -q ' /run/rufus-' /proc/self/mountinfo
    shopt -s nullglob
    temporary=(/run/rufus-*/ /tmp/rufus-prepare-helper.* /tmp/qt_temp.*)
    [[ ${#temporary[@]} == 0 ]]
    blockdev --setro /dev/sda
    blockdev --setro /dev/sda1
    [[ $(blockdev --getro /dev/sda) == 1 && $(blockdev --getro /dev/sda1) == 1 ]]
    echo RUFUS_WIM_SPLIT_READBACK_WRITE_PROTECTED
    "$checksum" /dev/sda > /home/rufus/created-disk.checksum
    fsck.fat -n /dev/sda1 > /home/rufus/fat-check.log 2>&1
    mount -t vfat -o ro /dev/sda1 /target
    (
        cd /target
        shopt -s globstar dotglob nullglob
        for path in **/*; do
            [[ ! -L $path ]]
            if [[ -d $path ]]; then
                printf '%s\0' "$path" >> /home/rufus/windows-directories
            elif [[ $path =~ ^sources/install([0-9]+)?\.swm$ ]]; then
                stat -c '%n %s' -- "$path" >> /home/rufus/split-parts.txt
            else
                sha256sum -z -- "$path" >> /home/rufus/windows-files.sha256z
            fi
        done
    )
    parts=(/target/sources/install[0-9]*.swm)
    /opt/rufus/wim_split_verify /target/sources/install.swm "${parts[@]}" \
        > /home/rufus/split-inventory.txt 2> >(tee /home/rufus/split-verify.log >&2)
    umount /target
    "$checksum" -c /home/rufus/created-disk.checksum
    echo RUFUS_WIM_SPLIT_READBACK_PASSED
fi
if [[ -f /etc/rufus-ntfs-fixture ]]; then
    shopt -s nullglob
    temporary=(/tmp/rufus-prepare-helper.* /tmp/prepare_helper_probe.* /tmp/prepare_boot_probe.* /tmp/prepare_extract_probe.* /tmp/qt_temp.*)
    [[ ${#temporary[@]} == 0 ]]
    echo RUFUS_BOOT_TEMPORARIES_REMOVED
    if [[ -f /etc/rufus-ntfs-mount ]]; then
        ! pgrep -f '^/usr/bin/lowntfs-3g-native '
        echo RUFUS_NTFS_DAEMONS_REAPED
        [[ $(< /home/rufus/test-exit) == 0 ]]
        # Enforce read-only verification below the filesystem as well. A ro
        # mount alone must not be able to change the just-created media.
        blockdev --setro /dev/sda
        blockdev --setro /dev/sda1
        [[ $(blockdev --getro /dev/sda) == 1 && $(blockdev --getro /dev/sda1) == 1 ]]
        echo RUFUS_NTFS_READBACK_WRITE_PROTECTED
        "$checksum" /dev/sda > /run/rufus-extracted-disk.checksum
        cp /run/rufus-extracted-disk.checksum /home/rufus/created-disk.checksum
        # Independent readback uses the existing kernel NTFS3 driver, even
        # when the new kernel driver or NTFS-3G performed creation.
        mount -t ntfs3 -o ro /dev/sda1 /target
        if [[ -f /etc/rufus-windows-fixture ]]; then
            if [[ -f /etc/rufus-windows-privacy ]]; then
                cp '/target/sources/$OEM$/$$/Panther/unattend.xml' /home/rufus/windows-unattend.xml
            fi
            # Independent NTFS3 readback of every file, without duplicating an
            # installation-sized tree in the guest system image.
            (
                cd /target
                shopt -s globstar dotglob nullglob
                for path in **/*; do
                    [[ ! -L $path ]]
                    if [[ -d $path ]]; then
                        printf '%s\0' "$path" >> /home/rufus/windows-directories
                    else
                        sha256sum -z -- "$path" >> /home/rufus/windows-files.sha256z
                    fi
                done
            )
        elif [[ -f /etc/rufus-large-file-path ]]; then
            large_path=$(< /etc/rufus-large-file-path)
            (cd /target && sha256sum "./$large_path") > /home/rufus/large-file.sha256
            stat -c %s "/target/$large_path" > /home/rufus/large-file.size
            tar -C /target --exclude="./$large_path" -cf /home/rufus/ntfs-tree.tar .
        else
            tar -C /target -cf /home/rufus/ntfs-tree.tar .
        fi
        umount /target
        "$checksum" -c /run/rufus-extracted-disk.checksum
        echo RUFUS_NTFS_EXTRACTION_READBACK
    fi
fi
kill "$monitor" 2>/dev/null || true
wait "$monitor" 2>/dev/null || true
if [[ -f /home/rufus/test-exit ]]; then
    echo "RUFUS_HELPER_TEST_EXIT $(< /home/rufus/test-exit)"
else
    echo RUFUS_HELPER_TEST_TIMEOUT
fi
echo "RUFUS_HELPER_AUTH_REQUESTS $(grep -c 'string "org.rufus.Prepare' /home/rufus/authorization.log)"
journalctl --no-pager -b -u rufus-gui.service -u polkit.service -n 240
echo RUFUS_HELPER_GUEST_FINISHED
systemctl poweroff
