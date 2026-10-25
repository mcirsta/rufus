#!/bin/sh
set -eu
family="$1"
remove_application() {
    if [ "$family" = debian ]; then
        DEBIAN_FRONTEND=noninteractive apt-get purge -y rufus-linux-preview
        ! dpkg-query -W -f='${Status}' rufus-linux-preview 2>/dev/null | grep -q 'install ok installed'
    else
        dnf -y --setopt=tsflags= remove rufus-linux-preview
        ! rpm -q rufus-linux-preview
    fi
    [ ! -e /usr/bin/rufus-linux ]
    [ ! -e /usr/lib/rufus-linux-preview/rufus-prepare-helper ]
    [ ! -e /usr/share/polkit-1/actions/org.rufus.PrepareDevice.policy ]
    [ ! -e /usr/share/applications/org.rufus.Rufus.desktop ]
    [ ! -e /usr/share/doc/rufus-linux-preview/README.md ]
    grep -q 'Preserve=cp79-user-state' /tmp/rufus-package-session/config/Rufus/Linux.ini
}
if [ "$family" = debian ]; then
    sha256sum /work/packages/*.deb /work/previous/*.deb > /work/qualified-package.sha256
    dpkg-deb --info /work/packages/*.deb > /work/package-info.txt
    dpkg-deb --contents /work/packages/*.deb > /work/package-files.txt
    printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
    chmod +x /usr/sbin/policy-rc.d
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        python3 patchelf desktop-file-utils passwd util-linux
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends /work/packages/*.deb
    dpkg-query -W rufus-linux-preview > /work/fresh-package-version.txt
else
    sha256sum /work/packages/*.rpm /work/previous/*.rpm > /work/qualified-package.sha256
    rpm -qip /work/packages/*.rpm > /work/package-info.txt
    rpm -qlp /work/packages/*.rpm > /work/package-files.txt
    rpm -qp --requires /work/packages/*.rpm > /work/package-requires.txt
    dnf -y --setopt=tsflags= --setopt=install_weak_deps=False install \
        python3 patchelf desktop-file-utils shadow-utils util-linux
    dnf -y --setopt=tsflags= --setopt=install_weak_deps=False install /work/packages/*.rpm
    rpm -q rufus-linux-preview > /work/fresh-package-version.txt
fi
useradd -m -u 1000 rufustest
python3 /harness/inspect-installed.py "$family" core 48
if [ "$family" = debian ]; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        dosfstools exfatprogs udftools ntfs-3g mtools
else
    dnf -y --setopt=tsflags= --setopt=install_weak_deps=False install \
        dosfstools exfatprogs udftools ntfs-3g ntfsprogs mtools
fi
python3 /harness/inspect-installed.py "$family" fresh 48
remove_application
if [ "$family" = debian ]; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends /work/previous/*.deb
    dpkg-query -W rufus-linux-preview > /work/previous-package-version.txt
else
    dnf -y --setopt=tsflags= --setopt=install_weak_deps=False install /work/previous/*.rpm
    rpm -q rufus-linux-preview > /work/previous-package-version.txt
fi
python3 /harness/inspect-installed.py "$family" previous 47
if [ "$family" = debian ]; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends /work/packages/*.deb
    dpkg-query -W rufus-linux-preview > /work/upgraded-package-version.txt
    dpkg-query -W > /work/runtime-environment-packages.txt
else
    dnf -y --setopt=tsflags= --setopt=install_weak_deps=False upgrade /work/packages/*.rpm
    rpm -q rufus-linux-preview > /work/upgraded-package-version.txt
    rpm -qa | sort > /work/runtime-environment-packages.txt
fi
python3 /harness/inspect-installed.py "$family" upgraded 48
remove_application
printf '%s\n' 'Fresh Preview48 installation/removal, published Preview47 to Preview48 upgrade, unprivileged runtime, provider paths, settings preservation and final removal passed.' > /work/transaction-result.txt
cat /etc/os-release > /work/runtime-os-release
