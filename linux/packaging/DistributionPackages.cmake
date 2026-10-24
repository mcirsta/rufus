# SPDX-License-Identifier: GPL-3.0-or-later
# Native packages use the libraries and tool paths of their build distribution.
# Select with cpack -G DEB or cpack -G RPM in a separately configured /usr build.
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/mcirsta/rufus")
set(CPACK_PACKAGE_CONTACT "Rufus Linux preview contributors")

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_VERSION "${RUFUS_UPSTREAM_VERSION}~preview${RUFUS_PREVIEW_REVISION}")
set(CPACK_DEBIAN_PACKAGE_RELEASE "1")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "udisks2, polkitd, pkexec, e2fsprogs, qt6-qpa-plugins")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "dosfstools, exfatprogs, udftools, ntfs-3g, mtools")

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
set(CPACK_RPM_PACKAGE_VERSION "${RUFUS_UPSTREAM_VERSION}")
set(CPACK_RPM_PACKAGE_RELEASE "0.preview${RUFUS_PREVIEW_REVISION}.1")
set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
set(CPACK_RPM_PACKAGE_REQUIRES "udisks2, polkit, e2fsprogs")
set(CPACK_RPM_PACKAGE_SUGGESTS "dosfstools, exfatprogs, udftools, ntfsprogs, ntfs-3g, mtools")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_RELOCATABLE OFF)
