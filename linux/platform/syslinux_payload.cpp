// SPDX-License-Identifier: GPL-3.0-or-later
#include "syslinux_payload.h"
#include "format_policy.h"
#include <QFile>
#include <cerrno>
#include <cstring>
extern "C"
{
#include "syslinux/libinstaller/syslinux.h"

// Existing Rufus patcher inputs. Only the verification call below sets pointers;
// the qualified embedded bytes, never disk contents, are used as patch templates.
unsigned char* syslinux_ldlinux[2];
unsigned long syslinux_ldlinux_len[2];
uint32_t SECTOR_SHIFT = 9;
uint32_t SECTOR_SIZE = 512;
}
namespace Syslinux
{
#include "syslinux/libinstaller/syslxfs.h"
}

static bool registerSyslinuxResources()
{
    Q_INIT_RESOURCE(syslinux_boot);
    return true;
}

namespace rufus
{
// Both hash-pinned payloads have 192 extent slots, even for one sector per extent.
static_assert(syslinux4Payload.paddedBytes / syslinuxSectorBytes - 1 <= 192);
static_assert(syslinux6Payload.paddedBytes / syslinuxSectorBytes - 1 <= 192);
static_assert(syslinuxAdvBytes == 2 * ADV_SIZE);

int CheckSyslinuxPayload(int bootType, int fileSystem, const QByteArray& beforeBoot,
    const QByteArray& afterBoot, const QByteArray& installed,
    const QList<uint64_t>& sectors, QString& problem, const SyslinuxResources* resources)
{
    const bool supportedBoot = bootType == BT_IMAGE || bootType == BT_SYSLINUX_V4 || bootType == BT_SYSLINUX_V6;
    const bool fat = fileSystem == FS_FAT16 || fileSystem == FS_FAT32;
    const bool ntfs = fileSystem == FS_NTFS && (bootType == BT_SYSLINUX_V6 || bootType == BT_IMAGE);
    if (!supportedBoot || (!fat && !ntfs))
        return EINVAL;
    const SyslinuxBootPayload selected = {resources ? int(resources->loader.size()) : 0, nullptr};
    const SyslinuxBootPayload& payload = resources ? selected : bootType == BT_SYSLINUX_V4 ? syslinux4Payload : syslinux6Payload;
    const bool bootSizes = beforeBoot.size() == syslinuxSectorBytes && afterBoot.size() == syslinuxSectorBytes;
    const bool fileSize = installed.size() == payload.installedBytes;
    const bool mapSize = sectors.size() >= payload.installedBytes / syslinuxSectorBytes;
    if (!bootSizes || !fileSize || !mapSize)
        return EBADMSG;
    static const bool registered = registerSyslinuxResources();
    Q_UNUSED(registered);
    const QString prefix = resources ? QString() : QString::fromLatin1(payload.resourcePrefix);
    QFile loaderFile(prefix + QStringLiteral("ldlinux.sys"));
    QFile bootFile(prefix + QStringLiteral("ldlinux.bss"));
    if (!resources && (!loaderFile.open(QIODevice::ReadOnly) || !bootFile.open(QIODevice::ReadOnly))) return EIO;
    QByteArray expected = resources ? resources->loader : loaderFile.readAll();
    QByteArray bootTemplate = resources ? resources->boot : bootFile.readAll();
    if (expected.size() != payload.loaderBytes || bootTemplate.size() != syslinuxSectorBytes)
        return EIO;
    expected.append(QByteArray(payload.paddedBytes - payload.loaderBytes, '\0'));
    unsigned char adv[2 * ADV_SIZE];
    syslinux_reset_adv(adv);
    QByteArray expectedBoot = beforeBoot;
    syslinux_ldlinux[0] = reinterpret_cast<unsigned char*>(expected.data());
    syslinux_ldlinux[1] = reinterpret_cast<unsigned char*>(bootTemplate.data());
    syslinux_ldlinux_len[0] = payload.paddedBytes;
    syslinux_ldlinux_len[1] = syslinuxSectorBytes;
    const int installedSectors = (payload.paddedBytes + sizeof(adv)) / syslinuxSectorBytes;
    const int patched = syslinux_patch(sectors.constData(), installedSectors, 0, 0, fileSystem == FS_NTFS ? "/" : nullptr, nullptr);
    syslinux_make_bootsect(expectedBoot.data(), fileSystem == FS_NTFS ? Syslinux::NTFS : Syslinux::VFAT);
    memset(syslinux_ldlinux, 0, sizeof(syslinux_ldlinux));
    memset(syslinux_ldlinux_len, 0, sizeof(syslinux_ldlinux_len));
    expected.append(reinterpret_cast<const char*>(adv), sizeof(adv));
    const bool loaderMatches = patched == payload.paddedBytes && expected == installed;
    if (!loaderMatches)
    {
        problem = QStringLiteral("Installed ldlinux.sys differs from the Rufus payload patched for this sector map.");
        return EBADMSG;
    }
    if (expectedBoot != afterBoot)
    {
        problem = QStringLiteral("Installed boot sector differs from the expected Rufus boot code and original BPB.");
        return EBADMSG;
    }
    return 0;
}
}
