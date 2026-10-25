#!/bin/sh
set -eu
family="$1"
cmake -S /source/linux -B /work/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBEXECDIR=lib/rufus-linux-preview -DBUILD_TESTING=OFF \
    -DRUFUS_WIMLIB_UPSTREAM=/upstream/wimlib-1.14.5 \
    -DRUFUS_SYSLINUX_UPSTREAM=/upstream/syslinux-6.04-pre1 \
    -DRUFUS_SYSLINUX4_UPSTREAM=/upstream/syslinux-4.07
cmake --build /work/build
mkdir -p /work/packages
if [ "$family" = debian ]; then
    cpack --config /work/build/CPackConfig.cmake -G DEB -B /work/packages
    dpkg-query -W > /work/build-environment-packages.txt
else
    cpack --config /work/build/CPackConfig.cmake -G RPM -B /work/packages
    rpm -qa | sort > /work/build-environment-packages.txt
fi
cat /etc/os-release > /work/build-os-release
cp /work/build/CMakeCache.txt /work/configured-provider-paths.txt
