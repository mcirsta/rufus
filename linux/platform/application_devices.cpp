// SPDX-License-Identifier: GPL-3.0-or-later
#include "application_devices.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <cerrno>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <cstring>
#include <fcntl.h>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <libmount/libmount.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr int maximumBackingDevices = 64, maximumDepth = 16;
constexpr qsizetype maximumAttributeBytes = 4096;
QByteArray attribute(const QString& path)
{
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray bytes = file.read(maximumAttributeBytes+1);
    if (file.error()!=QFileDevice::NoError || bytes.size()>maximumAttributeBytes || bytes.contains('\0')) return {};
    if (bytes.endsWith('\n')) bytes.chop(1);
    return bytes;
}
quint64 deviceNumber(const QString& path)
{
    const auto fields = attribute(path).split(':');
    if (fields.size()!=2) return 0;
    bool validMajor = false, validMinor = false;
    const uint deviceMajor = fields[0].toUInt(&validMajor), deviceMinor = fields[1].toUInt(&validMinor);
    const quint64 device = quint64(makedev(deviceMajor,deviceMinor));
    const bool exact = fields[0]==QByteArray::number(deviceMajor) && fields[1]==QByteArray::number(deviceMinor);
    return validMajor && validMinor && exact && major(dev_t(device))==deviceMajor && minor(dev_t(device))==deviceMinor ? device : 0;
}
int btrfsMembers(int fd, QSet<quint64>& devices, const QString& root)
{
    devices.clear();
    btrfs_ioctl_fs_info_args info = {};
    if (ioctl(fd, BTRFS_IOC_FS_INFO, &info) != 0) return errno;
    const QString uuid = QUuid::fromRfc4122(QByteArray(reinterpret_cast<const char*>(info.fsid), 16)).toString(QUuid::WithoutBraces);
    const QDir members(root + QStringLiteral("/fs/btrfs/") + uuid + QStringLiteral("/devices"));
    const auto entries = members.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (entries.isEmpty() || entries.size() > maximumBackingDevices) return ENOTSUP;
    for (const QString& entry : entries) {
        const quint64 device = deviceNumber(members.filePath(entry + QStringLiteral("/dev")));
        if (device == 0 || devices.contains(device)) return EBADMSG;
        devices.insert(device);
    }
    return quint64(entries.size()) == info.num_devices ? 0 : ENOTSUP;
}
int fileBackings(int fd, QSet<quint64>& devices, const QString& root, int depth);

int addBacking(quint64 device,QSet<quint64>& devices,const QString& root,int depth)
{
    if (device==0 || major(dev_t(device))==0) return ENOTSUP;
    if (devices.contains(device)) return 0;
    if (depth>maximumDepth || devices.size()>=maximumBackingDevices) return ELOOP;
    devices.insert(device);
    const QString path = QFileInfo(root+QStringLiteral("/dev/block/%1:%2").arg(major(dev_t(device))).arg(minor(dev_t(device)))).canonicalFilePath();
    if (path.isEmpty() || !path.startsWith(QDir(root).absolutePath()+QLatin1Char('/'))) return ENOENT;
    int error = 0;
    if (QFile::exists(path+QStringLiteral("/partition"))) {
        const quint64 parent = deviceNumber(QFileInfo(path).dir().filePath(QStringLiteral("dev")));
        error = addBacking(parent,devices,root,depth+1);
    }
    const auto slaves = QDir(path+QStringLiteral("/slaves")).entryList(QDir::Dirs|QDir::NoDotAndDotDot);
    for (const QString& slave : slaves) {
        const int next = addBacking(deviceNumber(path+QStringLiteral("/slaves/")+slave+QStringLiteral("/dev")),devices,root,depth+1);
        if (error==0) error = next;
    }
    if (QFile::exists(path+QStringLiteral("/loop"))) {
        const QByteArray backing = attribute(path+QStringLiteral("/loop/backing_file"));
        struct stat file = {};
        const bool absolute = backing.startsWith('/') && !backing.contains('\0');
        if (!absolute || stat(backing.constData(),&file)!=0) { if (error==0) error = ENOENT; }
        else {
            const int fd = open(backing.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            const int next = fd < 0 ? errno : fileBackings(fd, devices, root, depth + 1);
            if (fd >= 0) close(fd);
            if (error == 0) error = next;
        }
    }
    if (QFileInfo(path).fileName().startsWith(QStringLiteral("nbd"))) {
        QStringList sources, identity;
        const int resolved = NativeFileDiskSources(path, sources, identity);
        if (error == 0) error = resolved;
        for (const QString& source : sources) {
            const int fd = open(QFile::encodeName(source).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            const int next = fd < 0 ? errno : fileBackings(fd, devices, root, depth + 1);
            if (fd >= 0) close(fd);
            if (error == 0) error = next;
        }
    }
    return error;
}
int pathBackings(const QByteArray& path, QSet<quint64>& devices, const QString& root, int depth)
{
    if (!path.startsWith('/') || path.contains('\0')) return ENOTSUP;
    const int fd = open(path.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno;
    const int error = fileBackings(fd, devices, root, depth + 1);
    close(fd);
    return error;
}

int anonymousBackings(int fd, QSet<quint64>& devices, const QString& root, int depth)
{
    struct statx identity = {};
    if (statx(fd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_MNT_ID, &identity) != 0) return errno;
    if (!(identity.stx_mask & STATX_MNT_ID)) return ENOTSUP;
    libmnt_table* table = mnt_new_table();
    libmnt_iter* iterator = mnt_new_iter(MNT_ITER_FORWARD);
    if (!table || !iterator) { mnt_free_table(table); mnt_free_iter(iterator); return ENOMEM; }
    int error = -mnt_table_parse_file(table, "/proc/thread-self/mountinfo");
    QByteArray type, source, options;
    libmnt_fs* fs = nullptr;
    while (error == 0 && mnt_table_next_fs(table, iterator, &fs) == 0) {
        if (quint64(mnt_fs_get_id(fs)) != identity.stx_mnt_id) continue;
        type = mnt_fs_get_fstype(fs); source = mnt_fs_get_source(fs);
        options = mnt_fs_get_fs_options(fs); break;
    }
    mnt_free_iter(iterator); mnt_free_table(table);
    if (error != 0) return error;
    const bool memory = type == "tmpfs" || type == "ramfs";
    const bool remote = type == "nfs" || type == "nfs4" || type == "cifs" || type == "smb3";
    if (memory) return 0;
    if (remote) return CheckNativeRemoteBacking(options);
    if (type == "overlay") {
        bool found = false;
        for (const char* key : {"lowerdir", "upperdir", "workdir"}) {
            char* value = nullptr; size_t length = 0;
            if (mnt_optstr_get_option(options.constData(), key, &value, &length) != 0) continue;
            const QByteArray paths(value, qsizetype(length));
            // Escaped separators and detached/new mount API layers need a kernel
            // file handle. Never guess a path from an ambiguous option spelling.
            if (paths.contains('\\') || paths.isEmpty()) return ENOTSUP;
            for (const QByteArray& path : paths.split(':')) {
                const int next = pathBackings(path, devices, root, depth);
                if (next != 0) return next;
                found = true;
            }
        }
        return found ? 0 : ENOTSUP;
    }
    if (type.startsWith("fuse.") && source.startsWith('/'))
        return pathBackings(source, devices, root, depth);
    return ENOTSUP;
}

int fileBackings(int fd, QSet<quint64>& devices, const QString& root, int depth)
{
    if (depth > maximumDepth) return ELOOP;
    struct stat file = {};
    if (fstat(fd, &file) != 0) return errno;
    if (S_ISBLK(file.st_mode)) return addBacking(quint64(file.st_rdev), devices, root, depth + 1);
    if (!S_ISREG(file.st_mode) && !S_ISDIR(file.st_mode)) return ENOTSUP;
    if (major(file.st_dev) != 0) return addBacking(quint64(file.st_dev), devices, root, depth + 1);
    // Home is retained with O_PATH. Reopen that retained inode for statfs/ioctl.
    const QByteArray retained = QByteArray("/proc/self/fd/") + QByteArray::number(fd);
    const int readable = open(retained.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (readable < 0) return errno;
    struct statfs filesystem = {};
    int error = fstatfs(readable, &filesystem) == 0 ? 0 : errno;
    const bool memory = error == 0 && (filesystem.f_type == TMPFS_MAGIC || filesystem.f_type == RAMFS_MAGIC);
    if (memory) { close(readable); return 0; }
    if (error == 0 && filesystem.f_type == BTRFS_SUPER_MAGIC) {
        QSet<quint64> members;
        error = btrfsMembers(readable, members, root);
        for (quint64 member : members) {
            const int next = addBacking(member, devices, root, depth + 1);
            if (error == 0) error = next;
        }
    } else if (error == 0) error = anonymousBackings(fd, devices, root, depth);
    close(readable);
    return error;
}
}
int CheckNativeRemoteBacking(const QByteArray& mountOptions)
{
    char* value = nullptr; size_t length = 0;
    if (mnt_optstr_get_option(mountOptions.constData(), "addr", &value, &length) != 0 || length == 0 || length > 64) return ENOTSUP;
    QByteArray address(value, qsizetype(length));
    if (address.startsWith('[') && address.endsWith(']')) address = address.mid(1, address.size() - 2);
    const qsizetype scope = address.indexOf('%');
    if (scope >= 0) address.truncate(scope);
    in6_addr server = {}, v6 = {};
    in_addr v4 = {};
    if (inet_pton(AF_INET, address.constData(), &v4) == 1) {
        server.s6_addr[10] = server.s6_addr[11] = 0xff; memcpy(server.s6_addr + 12, &v4, 4);
    } else if (inet_pton(AF_INET6, address.constData(), &v6) == 1) server = v6;
    else return ENOTSUP;
    if (IN6_IS_ADDR_UNSPECIFIED(&server) || IN6_IS_ADDR_LOOPBACK(&server) || IN6_IS_ADDR_MULTICAST(&server)) return ENOTSUP;
    if (IN6_IS_ADDR_V4MAPPED(&server)) {
        const unsigned first = server.s6_addr[12];
        if (first == 0 || first == 127 || first >= 224) return ENOTSUP;
    }
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return ENOTSUP;
    bool local = false;
    for (const ifaddrs* interface = interfaces; interface; interface = interface->ifa_next) {
        if (!interface->ifa_addr) continue;
        in6_addr current = {};
        if (interface->ifa_addr->sa_family == AF_INET) {
            const auto* address = reinterpret_cast<const sockaddr_in*>(interface->ifa_addr);
            current.s6_addr[10] = current.s6_addr[11] = 0xff; memcpy(current.s6_addr + 12, &address->sin_addr, 4);
        } else if (interface->ifa_addr->sa_family == AF_INET6) {
            const auto* address = reinterpret_cast<const sockaddr_in6*>(interface->ifa_addr); current = address->sin6_addr;
        } else continue;
        if (memcmp(&current, &server, sizeof(server)) == 0) { local = true; break; }
    }
    freeifaddrs(interfaces);
    return local ? ENOTSUP : 0;
}
int NativeFileDiskSources(const QString& sysfsPath, QStringList& paths, QStringList& identity, bool metadataOnly)
{
    paths.clear(); identity.clear();
    const QString name = QFileInfo(sysfsPath).fileName();
    const bool loop = name.startsWith(QStringLiteral("loop")) && QFile::exists(sysfsPath + QStringLiteral("/loop/backing_file"));
    if (loop) {
        const QByteArray path = attribute(sysfsPath + QStringLiteral("/loop/backing_file"));
        if (!path.startsWith('/')) return ENOTSUP;
        paths.append(QFile::decodeName(path));
        identity.append({QString::fromLatin1(attribute(sysfsPath + QStringLiteral("/loop/offset"))),
            QString::fromLatin1(attribute(sysfsPath + QStringLiteral("/loop/sizelimit")))});
    } else if (name.startsWith(QStringLiteral("nbd"))) {
        bool valid = false;
        const quint64 pid = attribute(sysfsPath + QStringLiteral("/pid")).toULongLong(&valid);
        if (!valid || pid == 0 || pid > INT_MAX) return ENOTSUP;
        const QString process = QStringLiteral("/proc/%1").arg(pid);
        const QByteArray stat = attribute(process + QStringLiteral("/stat"));
        const auto fields = stat.mid(stat.lastIndexOf(')') + 2).split(' ');
        if (fields.size() <= 19 || fields[19].isEmpty()) return ESTALE;
        identity.append({QString::number(pid), QString::fromLatin1(fields[19])});
        if (metadataOnly) return 0;
        const QDir descriptors(process + QStringLiteral("/fd"));
        const auto entries = descriptors.entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
        if (entries.size() > 1024) return E2BIG;
        const quint64 exportDevice = deviceNumber(sysfsPath + QStringLiteral("/dev"));
        for (const QString& entry : entries) {
            bool numeric = false; const int number = entry.toInt(&numeric);
            if (!numeric || number < 3) continue;
            const QString path = descriptors.filePath(entry);
            struct stat file = {};
            if (::stat(QFile::encodeName(path).constData(), &file) != 0) return ESTALE;
            const bool backingBlock = S_ISBLK(file.st_mode) && quint64(file.st_rdev) != exportDevice;
            if ((S_ISREG(file.st_mode) && file.st_size > 0) || backingBlock) paths.append(path);
        }
        // A socket alone does not identify its server's backing file. Such NBD
        // clients remain opaque; never claim their local files are protected.
        if (paths.isEmpty()) return ENOTSUP;
    } else return ENOTSUP;
    for (const QString& path : paths) {
        struct stat file = {};
        if (::stat(QFile::encodeName(path).constData(), &file) != 0) return errno;
        if (!S_ISREG(file.st_mode)) return ENOTSUP;
        identity.append({path, QString::number(quint64(file.st_dev)), QString::number(quint64(file.st_ino)),
            QString::number(quint64(file.st_size))});
    }
    return 0;
}
int NativeBlockBackings(quint64 device,QSet<quint64>& devices,const QString& sysfsRoot)
{
    devices.clear();
    if (!QDir::isAbsolutePath(sysfsRoot) || sysfsRoot.contains(QChar::Null)) return EINVAL;
    const QString root = QDir(sysfsRoot).canonicalPath();
    if (root.isEmpty()) return ENOENT;
    return addBacking(device,devices,root,0);
}
int NativeBtrfsMembers(int descriptor, QSet<quint64>& devices)
{
    return btrfsMembers(descriptor, devices, QStringLiteral("/sys"));
}
int NativeFileBackings(int descriptor, QSet<quint64>& devices)
{
    devices.clear();
    return fileBackings(descriptor, devices, QStringLiteral("/sys"), 0);
}
QSet<quint64> NativeApplicationDevices()
{
    QSet<quint64> devices;
    for (const QByteArray& path : {QByteArray("/proc/self/exe"),QFile::encodeName(QCoreApplication::applicationDirPath())}) {
        const int fd = open(path.constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
        if (fd<0) continue;
        fileBackings(fd, devices, QStringLiteral("/sys"), 0); close(fd);
    }
    return devices;
}
}
