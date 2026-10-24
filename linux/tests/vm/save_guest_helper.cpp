// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/save_source.h"
#include <QFile>
#include <fcntl.h>
#include <unistd.h>
extern "C" int __real_umount(const char*);
extern "C" int __wrap_umount(const char* path)
{
    QFile fault(QStringLiteral("/run/save-helper-fault"));
    if (fault.open(QIODevice::ReadOnly) && fault.readAll() == "cleanup" && QByteArray(path).startsWith("/run/rufus-save-")) {
        errno = EBUSY; return -1;
    }
    return __real_umount(path);
}
int main(int argc, char** argv)
{
    QFile marker(QStringLiteral("/etc/rufus-save-export-fixture")), dmi(QStringLiteral("/sys/class/dmi/id/product_name"));
    if (getuid() != 0 || geteuid() != 0 || !marker.open(QIODevice::ReadOnly) || marker.readAll() != "save-export\n"
        || !dmi.open(QIODevice::ReadOnly) || dmi.readAll() != "RufusMBRFixture\n") return 77;
    const int result = rufus::RunSaveSourceHelper(argc, argv);
    const int held = open("/run/save-exit-held", O_CREAT | O_WRONLY, 0644);
    if (held >= 0) close(held);
    usleep(250000);
    unlink("/run/save-exit-held");
    return result;
}
