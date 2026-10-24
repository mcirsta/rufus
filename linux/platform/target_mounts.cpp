// SPDX-License-Identifier: GPL-3.0-or-later
#include "target_mounts.h"
#include "application_devices.h"
#include "rufus_reporting.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <libmount/libmount.h>
#include <linux/fs.h>
#include <linux/btrfs.h>
#include <pwd.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace rufus
{
namespace
{
struct KernelMount
{
    NativeTargetMount volume;
    int parent = 0;
    QByteArray root;
    QByteArray type;
    QByteArray source;
};

int parseError(libmnt_table*, const char*, int)
{
    // libmount normally skips malformed lines. Missing a mount is unsafe here.
    return -EBADMSG;
}

int readMounts(QList<KernelMount>& mounts)
{
    libmnt_table* table = mnt_new_table();
    libmnt_iter* iterator = mnt_new_iter(MNT_ITER_FORWARD);
    if (!table || !iterator)
    {
        mnt_free_table(table);
        mnt_free_iter(iterator);
        return ENOMEM;
    }
    mnt_table_set_parser_errcb(table, parseError);
    int error = -mnt_table_parse_file(table, "/proc/thread-self/mountinfo");
    libmnt_fs* fs = nullptr;
    while (error == 0 && mnt_table_next_fs(table, iterator, &fs) == 0)
    {
        KernelMount mount;
        mount.volume.path = mnt_fs_get_target(fs);
        mount.volume.device = quint64(mnt_fs_get_devno(fs));
        mount.volume.id = mnt_fs_get_id(fs);
        mount.parent = mnt_fs_get_parent_id(fs);
        mount.root = mnt_fs_get_root(fs);
        mount.type = mnt_fs_get_fstype(fs);
        mount.source = mnt_fs_get_source(fs);
        const bool identified = mount.volume.id > 0 && mount.parent > 0;
        const bool absolute = mount.volume.path.startsWith('/') && mount.root.startsWith('/');
        if (!identified || !absolute || mount.type.isEmpty())
            error = EBADMSG;
        else
            mounts.append(mount);
    }
    mnt_free_iter(iterator);
    mnt_free_table(table);
    if (error == 0 && mounts.isEmpty())
        error = EBADMSG;
    return error;
}

QString sysfsPath(quint64 device)
{
    return QStringLiteral("/sys/dev/block/%1:%2").arg(major(dev_t(device))).arg(minor(dev_t(device)));
}

int checkNoLayers(const QString& path)
{
    for (const char* name : {"holders", "slaves"})
    {
        // Kernel partitions have holders, but no slaves directory of their own.
        if (strcmp(name, "slaves") == 0 && QFile::exists(path + QStringLiteral("/partition")))
            continue;
        const QByteArray directory = QFile::encodeName(path + QLatin1Char('/') + QLatin1String(name));
        DIR* entries = opendir(directory.constData());
        if (!entries)
            return errno;
        errno = 0;
        int error = 0;
        dirent* entry;
        while ((entry = readdir(entries)) != nullptr)
        {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            {
                error = EBUSY;
                break;
            }
        }
        if (error == 0)
            error = errno;
        closedir(entries);
        if (error != 0)
            return error;
    }
    return 0;
}

int targetMembers(quint64 disk, QSet<quint64>& members)
{
    const QString path = sysfsPath(disk);
    if (QFile::exists(path + QStringLiteral("/partition")))
        return ENOTBLK;
    if (QFileInfo(path).canonicalFilePath().startsWith(QStringLiteral("/sys/devices/virtual/block/nbd"))) {
        QStringList paths, identity;
        const int backing = NativeFileDiskSources(QFileInfo(path).canonicalFilePath(), paths, identity);
        if (backing != 0) return backing;
    }
    int error = checkNoLayers(path);
    if (error != 0)
        return error;
    members.insert(disk);
    const QStringList entries = QDir(path).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries)
    {
        const QString child = path + QLatin1Char('/') + entry;
        if (!QFile::exists(child + QStringLiteral("/partition")))
            continue;
        QFile file(child + QStringLiteral("/dev"));
        if (!file.open(QIODevice::ReadOnly))
            return EIO;
        const QList<QByteArray> numbers = file.read(128).trimmed().split(':');
        if (numbers.size() != 2)
            return EBADMSG;
        bool majorValid = false, minorValid = false;
        const uint majorNumber = numbers[0].toUInt(&majorValid);
        const uint minorNumber = numbers[1].toUInt(&minorValid);
        if (!majorValid || !minorValid)
            return EBADMSG;
        error = checkNoLayers(child);
        if (error != 0)
            return error;
        members.insert(quint64(makedev(majorNumber, minorNumber)));
    }
    return 0;
}

int checkBacking(int descriptor, bool directory, const QSet<quint64>& members, const QList<KernelMount>& mounts)
{
    if (descriptor < 0)
        return descriptor == -1 ? 0 : EBADF;
    struct stat info;
    if (fstat(descriptor, &info) != 0)
        return errno;
    const bool correctType = directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode);
    if (!correctType)
        return EINVAL;
    Q_UNUSED(mounts);
    // A retained image may be the very file exported by a loop/NBD target.
    // Its filesystem dev_t alone cannot reveal that alias.
    for (quint64 member : members) {
        QStringList paths, identity;
        const QString native = QFileInfo(sysfsPath(member)).canonicalFilePath();
        if (NativeFileDiskSources(native, paths, identity) != 0) continue;
        for (const QString& path : paths) {
            struct stat file = {};
            if (stat(QFile::encodeName(path).constData(), &file) != 0) return ESTALE;
            if (file.st_dev == info.st_dev && file.st_ino == info.st_ino) return EBUSY;
        }
    }
    QSet<quint64> backing;
    const int error = NativeFileBackings(descriptor, backing);
    for (quint64 device : backing)
        if (members.contains(device)) return EBUSY;
    return error;
}

int checkSwap(const QSet<quint64>& members)
{
    libmnt_table* table = mnt_new_table();
    libmnt_iter* iterator = mnt_new_iter(MNT_ITER_FORWARD);
    if (!table || !iterator)
    {
        mnt_free_table(table);
        mnt_free_iter(iterator);
        return ENOMEM;
    }
    mnt_table_set_parser_errcb(table, parseError);
    int error = -mnt_table_parse_swaps(table, "/proc/swaps");
    libmnt_fs* fs = nullptr;
    while (error == 0 && mnt_table_next_fs(table, iterator, &fs) == 0)
    {
        struct stat info;
        const char* path = mnt_fs_get_source(fs);
        if (!path || stat(path, &info) != 0)
            error = path ? errno : EBADMSG;
        else
        {
            const quint64 device = quint64(S_ISBLK(info.st_mode) ? info.st_rdev : info.st_dev);
            if (members.contains(device))
                error = EBUSY;
            else if (!S_ISBLK(info.st_mode) && major(info.st_dev) == 0)
                error = ENOTSUP;
        }
    }
    mnt_free_iter(iterator);
    mnt_free_table(table);
    return error;
}

int selectMounts(const QList<KernelMount>& mounts, const QSet<quint64>& members,
    QList<NativeTargetMount>& selected)
{
    QSet<quint64> filesystems, backingDevices;
    QSet<int> mountIds;
    for (const KernelMount& mount : mounts)
    {
        NativeTargetMount volume = mount.volume;
        bool target = members.contains(mount.volume.device);
        if (!target && mount.source.startsWith("/dev/"))
        {
            struct stat info;
            if (stat(mount.source.constData(), &info) == 0 && S_ISBLK(info.st_mode))
                target = members.contains(quint64(info.st_rdev));
        }
        if (mount.type == "btrfs") {
            const int fd = open(mount.volume.path.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0) return errno;
            QSet<quint64> backing, direct;
            const int error = NativeFileBackings(fd, backing);
            const int identified = NativeBtrfsMembers(fd, direct);
            struct stat info = {};
            const int checked = fstat(fd, &info) == 0 ? 0 : errno;
            close(fd);
            for (quint64 device : backing) target = target || members.contains(device);
            if (target && error != 0) return error;
            if (target && identified != 0) return identified;
            if (target && checked != 0) return checked;
            if (target) {
                if (direct.size() != 1 || !members.contains(*direct.cbegin())) return EBUSY;
                volume.device = quint64(info.st_dev); volume.backing = *direct.cbegin();
            }
        }
        if (!target)
            continue;
        // fuseblk has the kernel's real block dev_t, just like native filesystems.
        // A FUSE source string alone never establishes that backing identity.
        const bool direct = members.contains(mount.volume.device) || mount.type == "btrfs";
        const bool supported = mount.type == "vfat" || mount.type == "exfat" || mount.type == "ntfs" || mount.type == "udf"
            || mount.type == "ntfs3" || mount.type == "fuseblk"
            || mount.type == "ext2" || mount.type == "ext3" || mount.type == "ext4" || mount.type == "btrfs";
        if (!direct || !supported)
            return ENOTSUP;
        const quint64 storage = volume.backing != 0 ? volume.backing : volume.device;
        const bool alias = (mount.root != "/" && mount.type != "btrfs") || filesystems.contains(volume.device) || backingDevices.contains(storage);
        if (alias || mount.volume.path == "/")
            return EBUSY;
        filesystems.insert(volume.device); backingDevices.insert(storage);
        mountIds.insert(volume.id);
        selected.append(volume);
    }
    // Any direct child implies a nested mount, even when its backing disk is
    // unrelated. Refuse the whole plan instead of recursively unmounting it.
    for (const KernelMount& mount : mounts)
        if (mountIds.contains(mount.parent))
            return EBUSY;
    for (const char* path : {"/", "/boot", "/etc", "/usr", "/var", "/home", "/run",
             "/dev", "/proc", "/sys", "/proc/self/exe", "."})
    {
        struct stat info;
        if (stat(path, &info) != 0)
        {
            if (errno == ENOENT)
                continue;
            return errno;
        }
        if (filesystems.contains(quint64(info.st_dev))) return EBUSY;
        const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return errno;
        QSet<quint64> backing;
        const int resolved = NativeFileBackings(fd, backing);
        close(fd);
        for (quint64 device : backing) if (members.contains(device)) return EBUSY;
        // Unknown unrelated proc/sysfs/anonymous mounts do not establish that
        // this target is their backing. Source/home checks require completeness.
        Q_UNUSED(resolved);
    }
    return 0;
}

bool sameMount(const NativeTargetMount& left, const NativeTargetMount& right)
{
    return left.id == right.id && left.device == right.device && left.backing == right.backing && left.path == right.path;
}

int unmountVolume(const NativeTargetMount& volume)
{
    const int slash = volume.path.lastIndexOf('/');
    if (slash < 0 || slash == volume.path.size() - 1 || volume.path.contains('\0'))
        return EINVAL;
    const QByteArray parentPath = slash == 0 ? QByteArray("/") : volume.path.left(slash);
    const QByteArray name = volume.path.mid(slash + 1);
    const int parent = open(parentPath.constData(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0)
        return errno;
    struct stat info;
    int error = fstat(parent, &info) == 0 ? 0 : errno;
    const bool trustedParent = error == 0 && info.st_uid == 0 && (info.st_mode & (S_IWGRP | S_IWOTH)) == 0;
    if (error == 0 && !trustedParent)
        error = EPERM;
    struct statx current = {};
    if (error == 0 && statx(parent, name.constData(), AT_SYMLINK_NOFOLLOW, STATX_MNT_ID | STATX_TYPE, &current) != 0)
        error = errno;
    const bool sameId = (current.stx_mask & STATX_MNT_ID) != 0 && current.stx_mnt_id == quint64(volume.id);
    const bool sameDevice = quint64(makedev(current.stx_dev_major, current.stx_dev_minor)) == volume.device;
    const bool directory = (current.stx_mode & S_IFMT) == S_IFDIR;
    if (error == 0 && (!sameId || !sameDevice || !directory))
        error = ESTALE;
    // Pin the parent without opening the mounted filesystem (which would make
    // our own unmount busy). NOFOLLOW protects the final component.
    const QByteArray retainedPath = QByteArray("/proc/self/fd/") + QByteArray::number(parent) + '/' + name;
    if (error == 0 && IsRufusJobCancelled())
        error = ECANCELED;
    if (error == 0 && umount2(retainedPath.constData(), UMOUNT_NOFOLLOW) != 0)
        error = errno;
    close(parent);
    return error;
}
}

int CreatePrivateMountNamespace()
{
    if (getuid() != 0 || geteuid() != 0)
        return EPERM;
    if (IsRufusJobCancelled())
        return ECANCELED;
    // CLONE_NEWNS also unshares filesystem attributes for this thread. Do not
    // unshare the descriptor table: the helper owns it until the worker exits.
    if (unshare(CLONE_NEWNS) != 0)
        return errno;
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0)
        return errno;
    return IsRufusJobCancelled() ? ECANCELED : 0;
}

int OpenNativeCallerHome(uid_t caller, int& descriptor)
{
    descriptor = -1;
    if (caller == 0 || caller == uid_t(-1))
        return EINVAL;
    constexpr int initialAccountBytes = 16384;
    constexpr int maximumAccountBytes = 1024 * 1024;
    QByteArray buffer(initialAccountBytes, '\0');
    passwd account = {};
    passwd* found = nullptr;
    int error;
    for (;;)
    {
        if (IsRufusJobCancelled())
            return ECANCELED;
        error = getpwuid_r(caller, &account, buffer.data(), size_t(buffer.size()), &found);
        if (error != ERANGE || buffer.size() >= maximumAccountBytes)
            break;
        buffer.resize(buffer.size() * 2);
    }
    if (error != 0)
        return error;
    if (!found)
        return ENOENT;
    if (account.pw_uid != caller || !account.pw_dir || account.pw_dir[0] != '/')
        return EINVAL;
    // Following a home symlink is intentional: protect its actual filesystem.
    const int home = open(account.pw_dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (home < 0)
        return errno;
    if (IsRufusJobCancelled())
    {
        close(home);
        return ECANCELED;
    }
    descriptor = home;
    return 0;
}

int CheckNativeTargetMounts(int disk, int source, NativeTargetMounts& result, int callerHome)
{
    result = {};
    if (IsRufusJobCancelled())
        return ECANCELED;
    struct stat info;
    if (fstat(disk, &info) != 0)
        return errno;
    if (!S_ISBLK(info.st_mode))
        return ENOTBLK;
    NativeTargetMounts checked;
    checked.device = quint64(info.st_rdev);
    uint64_t sequence = 0;
    if (ioctl(disk, BLKGETDISKSEQ, &sequence) != 0)
        return errno;
    checked.diskSequence = sequence;
    QSet<quint64> members;
    int error = targetMembers(checked.device, members);
    QList<KernelMount> mounts;
    if (error == 0)
        error = readMounts(mounts);
    if (error == 0)
        error = checkBacking(source, false, members, mounts);
    if (error == 0)
        error = checkBacking(callerHome, true, members, mounts);
    if (error == 0)
        error = checkSwap(members);
    if (error == 0)
        error = selectMounts(mounts, members, checked.volumes);
    if (error == 0 && ioctl(disk, BLKGETDISKSEQ, &sequence) != 0)
        error = errno;
    if (error == 0 && sequence != checked.diskSequence)
        error = ESTALE;
    if (error == 0 && IsRufusJobCancelled())
        error = ECANCELED;
    if (error == 0)
        result = checked;
    return error;
}

int UnmountNativeTarget(int disk, int source, const NativeTargetMounts& expected, int& unmounted, int callerHome)
{
    unmounted = 0;
    if (getuid() != 0 || geteuid() != 0)
        return EPERM;
    for (int index = 0; index <= expected.volumes.size(); index++)
    {
        NativeTargetMounts current;
        const int error = CheckNativeTargetMounts(disk, source, current, callerHome);
        if (error != 0)
            return error;
        const bool sameDisk = current.device == expected.device && current.diskSequence == expected.diskSequence;
        const bool sameCount = current.volumes.size() == expected.volumes.size() - index;
        if (!sameDisk || !sameCount)
            return ESTALE;
        for (int remaining = 0; remaining < current.volumes.size(); remaining++)
            if (!sameMount(current.volumes[remaining], expected.volumes[index + remaining]))
                return ESTALE;
        if (current.volumes.isEmpty())
            return 0;
        const int unmountError = unmountVolume(current.volumes.first());
        if (unmountError != 0)
            return unmountError;
        unmounted++;
    }
    return EIO;
}
}
