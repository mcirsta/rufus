#!/bin/sh
set -eu
family="$1"
sha256sum /work/build/rufus-linux /work/build/rufus-prepare-helper \
    /work/build/syslinux/rufus-syslinux /work/build/syslinux/rufus-syslinux4 \
    /work/build/syslinux/rufus-extlinux > /work/binaries-before-restage.sha256
cmake -S /source/linux -B /work/build
if [ "$family" = debian ]; then
    cpack --config /work/build/CPackConfig.cmake -G DEB -B /work/packages
else
    cpack --config /work/build/CPackConfig.cmake -G RPM -B /work/packages
fi
sha256sum -c /work/binaries-before-restage.sha256
