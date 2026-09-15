#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
export DISPLAY=:0
# The native KDE agent uses Qt Quick; use its software renderer under Xvfb.
export QT_QUICK_BACKEND=software
Xvfb :0 -screen 0 1280x900x24 -nolisten tcp &
xvfb_pid=$!
finish()
{
    printf '%s\n' "$?" > /home/rufus/test-exit
    kill "$xvfb_pid" 2>/dev/null || true
}
trap finish EXIT
# The test waits for an exposed window; this only lets Xvfb create its socket.
for i in {1..50}; do [ -S /tmp/.X11-unix/X0 ] && break; sleep .1; done
set -- nativeAuthenticationAndWrite nativeExclusiveAccessAndCancellation nativeRemovalDuringWrite nativeReattachment
if [ -f /opt/rufus/openwrt-bios.img ]; then set -- "$@" nativeLinuxImages; fi
/opt/rufus/test_usb_guest -v1 "$@"
