#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
[[ $(< /sys/class/dmi/id/product_name) == RufusUSBFixture ]]
[[ $(< /etc/rufus-installed-fixture) == RUFUS-INSTALLED-TEST ]]
trap 'echo "RUFUS_INSTALLED_SUPERVISOR_FAILED $LINENO"; systemctl poweroff' ERR
chmod 4755 /usr/bin/pkexec
chown -R 1000:1000 /home/rufus
modprobe -a xhci_pci usb_storage sd_mod vfat nls_cp437 nls_iso8859_1
systemctl start polkit.service udisks2.service
udevadm wait --timeout=10 /dev/sda
helper=$(< /etc/rufus-installed-helper)
clean_job()
{
    if pgrep -f "^$helper --device$"; then
        return 1
    fi
    if grep -q ' /run/rufus-' /proc/self/mountinfo; then
        return 1
    fi
    shopt -s nullglob
    temporary=(/run/rufus-*/ /tmp/rufus-prepare-helper.* /tmp/qt_temp.*)
    [[ ${#temporary[@]} == 0 ]]
}
stdbuf -oL dbus-monitor --system \
    "type='method_call',interface='org.freedesktop.PolicyKit1.Authority',member='CheckAuthorization'" \
    > /home/rufus/authorization.log 2>/home/rufus/authorization-errors.log &
monitor=$!
sleep .2
systemctl start --no-block rufus-gui.service
for ((attempt=0; attempt<4200; attempt++)); do
    if [[ -f /home/rufus/check-cleanup && ! -f /home/rufus/cleanup-ok ]]; then
        clean_job
        touch /home/rufus/cleanup-ok
        echo RUFUS_INSTALLED_CANCEL_CLEANUP_OK
    fi
    [[ ! -f /home/rufus/test-exit ]] || break
    sleep .1
done
systemctl stop rufus-gui.service || true
[[ -f /home/rufus/test-exit && $(< /home/rufus/test-exit) == 0 ]]
clean_job
kill "$monitor" 2>/dev/null || true
wait "$monitor" 2>/dev/null || true
requests=$(grep -c 'string "org.rufus.PrepareDevice"' /home/rufus/authorization.log)
[[ $requests == 2 ]]
echo "RUFUS_INSTALLED_RUNTIME_PASSED REQUESTS=$requests"
journalctl --no-pager -b -u rufus-gui.service -u polkit.service -n 200
systemctl poweroff
