#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
test "$(cat /sys/devices/virtual/dmi/id/product_name)" = RufusUSBFixture
test "$(cat /etc/rufus-usb-fixture)" = RUFUS-TEST-USB
chown -R 1000:1000 /home/rufus
systemctl start polkit.service udisks2.service
printf 'RUFUS_GUEST_READY\n'
check_exclusive()
{
    mkdir /run/rufus-exclusive
    for i in {1..900}; do [ -f /home/rufus/exclusive-request ] && break; sleep .1; done
    [ -f /home/rufus/exclusive-request ] || return
    /opt/rufus/check_usb_mount busy > /home/rufus/exclusive-result 2>/home/rufus/mount-error || return
    for i in {1..150}; do [ -f /home/rufus/exclusive-release ] && break; sleep .1; done
    [ -f /home/rufus/exclusive-release ] || return
    /opt/rufus/check_usb_mount released > /home/rufus/release-result 2>>/home/rufus/mount-error
}
check_exclusive &
mount_check=$!
snapshot_images()
{
    for firmware in bios uefi; do
        for i in {1..3000}; do [ -f "/home/rufus/snapshot-$firmware" ] && break; sleep .1; done
        [ -f "/home/rufus/snapshot-$firmware" ] || return
        cp --sparse=always /dev/sda "/home/rufus/openwrt-$firmware.raw"
        touch "/home/rufus/snapshot-$firmware-done"
    done
}
snapshot_images &
snapshot_check=$!
systemctl start --no-block rufus-gui.service
for i in {1..3000}; do [ -f /home/rufus/test-exit ] && break; sleep .1; done
if [ -f /home/rufus/test-exit ]; then
    printf 'RUFUS_GUI_EXIT %s\n' "$(cat /home/rufus/test-exit)"
else
    printf 'RUFUS_GUI_TIMEOUT\n'
    ps -ef
fi
systemctl stop rufus-gui.service || true
kill "$mount_check" 2>/dev/null || true
wait "$mount_check" 2>/dev/null || true
kill "$snapshot_check" 2>/dev/null || true
wait "$snapshot_check" 2>/dev/null || true
journalctl --no-pager -b -u rufus-gui.service -u polkit.service -u udisks2.service -n 160
printf 'RUFUS_GUEST_FINISHED\n'
systemctl poweroff
