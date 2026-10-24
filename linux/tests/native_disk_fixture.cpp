// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_disk_fixture.h"
#include <QMap>

static QMap<quint64, rufus::NativeDiskFacts> disks;
int testNativeDiskError = 0;

rufus::NativeDiskFacts& TestNativeDisk(quint64 device)
{
    if (!disks.contains(device))
    {
        rufus::NativeDiskFacts facts;
        facts.sysfsPath = QStringLiteral("/test/sys/devices/usb/%1").arg(device);
        facts.diskSequence = 1;
        facts.logicalSectorSize = 512;
        facts.physicalSectorSize = 512;
        facts.mediaRemoval = rufus::MediaRemoval::Removable;
        facts.vendor = "Test";
        facts.model = "USB";
        disks.insert(device, facts);
    }
    return disks[device];
}

void ResetTestNativeDisks()
{
    disks.clear();
    testNativeDiskError = 0;
}

extern "C" int __wrap__ZN5rufus19ReadNativeDiskFactsEyyRNS_15NativeDiskFactsERK7QString(
    quint64 device, quint64 size, rufus::NativeDiskFacts& facts, const QString&)
{
    facts = {};
    if (testNativeDiskError != 0)
        return testNativeDiskError;
    facts = TestNativeDisk(device);
    facts.size = size;
    return 0;
}
