#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
[[ $(< /sys/class/dmi/id/product_name) == RufusUSBFixture ]]
[[ $(< /etc/rufus-gpt-fixture) == RUFUS-GPT-TEST ]]
# The initramfs assigns package ownership, which clears setuid bits on Linux.
# Restore the packaged pkexec mode inside this isolated guest only.
chmod 4755 /usr/bin/pkexec
chown -R 1000:1000 /home/rufus
mkdir /target
modprobe -a xhci_pci usb_storage sd_mod vfat nls_cp437 nls_iso8859_1
systemctl start polkit.service
stdbuf -oL dbus-monitor --system \
    "type='method_call',interface='org.freedesktop.PolicyKit1.Authority',member='CheckAuthorization'" \
    > /home/rufus/authorization.log 2>/home/rufus/authorization-errors.log &
monitor=$!
sleep .2
systemctl start --no-block rufus-gui.service
for attempt in {1..1800}; do
    [[ -f /home/rufus/test-exit ]] && break
    sleep .1
done
systemctl stop rufus-gui.service || true
kill "$monitor" 2>/dev/null || true
wait "$monitor" 2>/dev/null || true
if [[ -f /home/rufus/test-exit ]]; then
    echo "RUFUS_HELPER_TEST_EXIT $(< /home/rufus/test-exit)"
else
    echo RUFUS_HELPER_TEST_TIMEOUT
fi
echo "RUFUS_HELPER_AUTH_REQUESTS $(grep -c 'string "org.rufus.TestCreate"' /home/rufus/authorization.log)"
journalctl --no-pager -b -u rufus-gui.service -u polkit.service -n 180
echo RUFUS_HELPER_GUEST_FINISHED
systemctl poweroff
