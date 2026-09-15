#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
[[ $(< /sys/class/dmi/id/product_name) == RufusUSBFixture ]]
[[ $(< /etc/rufus-gpt-fixture) == RUFUS-GPT-TEST ]]
export DISPLAY=:0 QT_QUICK_BACKEND=software
Xvfb :0 -screen 0 1280x900x24 -nolisten tcp &
xvfb=$!
finish()
{
    echo "$?" > /home/rufus/test-exit
    kill "$xvfb" 2>/dev/null || true
}
trap finish EXIT
for attempt in {1..50}; do
    [[ -S /tmp/.X11-unix/X0 ]] && break
    sleep .1
done
/opt/rufus/test_helper_guest -v1
