// SPDX-License-Identifier: GPL-3.0-or-later
#include "save_source.h"
#include "device_exclusions.h"
#include "native_disk_facts.h"
#include "application_devices.h"
#include "image_save.h"
#include "device_selection.h"
#include "target_mounts.h"
#include "rufus_reporting.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QVersionNumber>
#include <QSocketNotifier>
#include <QThread>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace rufus {
namespace {
constexpr int messageBytes = 8192;
constexpr int pollMs = 25;
quint64 number(const QJsonObject& object, const char* key, bool& valid)
{
    const QString text = object.value(QLatin1String(key)).toString();
    bool parsed = false;
    const quint64 result = text.toULongLong(&parsed);
    valid = valid && parsed && QString::number(result) == text;
    return result;
}
QByteArray sysfs(quint64 device) { return QByteArray("/sys/dev/block/") + QByteArray::number(major(dev_t(device))) + ':' + QByteArray::number(minor(dev_t(device))); }
struct Descriptor {
    int fd = -1;
    ~Descriptor() { close(); }
    int close() { if (fd < 0) return 0; const int value = fd; fd = -1; return ::close(value) == 0 ? 0 : errno; }
};
class SourceThread final : public QThread {
public:
    SaveSourceRequest request;
    uid_t caller = 0;
    std::atomic<bool> release{false};
    int error = EIO, unmounted = 0;
protected:
    void run() override
    {
        Descriptor channel, disk, home, directory;
        QByteArray mountPath;
        bool mounted = false;
        QProcess ntfs;
        try {
            channel.fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
            sockaddr_un address = {}; address.sun_family = AF_UNIX;
            const QByteArray path = QFile::encodeName(request.socket);
            error = channel.fd < 0 ? errno : 0;
            if (error == 0 && (path.size() >= int(sizeof(address.sun_path)) || path.contains('\0'))) error = EINVAL;
            if (error == 0) {
                memcpy(address.sun_path, path.constData(), size_t(path.size() + 1));
                if (::connect(channel.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) error = errno;
            }
            ucred peer = {}; socklen_t peerSize = sizeof(peer);
            if (error == 0 && getsockopt(channel.fd, SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) != 0) error = errno;
            if (error == 0 && (peerSize != sizeof(peer) || peer.uid != caller)) error = EPERM;
            if (error == 0) error = acquire(disk, home, directory, mountPath, mounted, ntfs);
            QJsonObject metadata{{"token", QString::fromLatin1(request.token)}, {"error", error}, {"iso", request.iso}};
            unsigned sector = 0;
            if (error == 0) error = CheckImageSaveDevice(disk.fd, request.source.device, request.source.size, request.source.diskSequence, sector);
        if (error == 0) error = CheckDeviceExclusions(request.source.path, request.source.device, request.source.size, request.source.exclusions, disk.fd);
        if (error == 0 && request.source.imageInode != 0) {
            NativeDiskFacts facts;
            error = ReadNativeDiskFacts(request.source.device, request.source.size, facts);
            if (error == 0 && !facts.backingIdentity.isEmpty()) {
                QStringList paths, identity;
                error = NativeFileDiskSources(facts.sysfsPath, paths, identity);
                for (const QString& path : paths) {
                    struct stat file = {};
                    if (stat(QFile::encodeName(path).constData(), &file) != 0) { error = ESTALE; break; }
                    if (quint64(file.st_dev) == request.source.imageDevice && quint64(file.st_ino) == request.source.imageInode) { error = EBUSY; break; }
                }
            }
        }
            struct stat volume = {};
            if (error == 0 && request.iso && fstat(directory.fd, &volume) != 0) error = errno;
            metadata.insert(QStringLiteral("error"), error);
            metadata.insert(QStringLiteral("sector"), int(sector));
            metadata.insert(QStringLiteral("volumeDevice"), QString::number(quint64(volume.st_dev)));
            metadata.insert(QStringLiteral("label"), label_);
            if (channel.fd >= 0) {
                const int sent = SendSaveSourceDescriptor(channel.fd, error == 0 ? (request.iso ? directory.fd : disk.fd) : -1, metadata);
                if (sent != 0 && error == 0) error = sent;
            }
            while (error == 0 && !release.load() && !isInterruptionRequested()) {
                pollfd peerPoll{channel.fd, POLLIN | POLLHUP, 0};
                if (poll(&peerPoll, 1, pollMs) < 0 && errno != EINTR) error = errno;
                else if (peerPoll.revents & (POLLHUP | POLLERR | POLLNVAL)) error = ECANCELED;
            }
            if (error == 0 && isInterruptionRequested()) error = ECANCELED;
            if (error == 0) error = CheckImageSaveDevice(disk.fd, request.source.device, request.source.size, request.source.diskSequence, sector);
        } catch (...) { error = EIO; }
        // Keep every cleanup failure visible, including after cancellation.
        int cleanup = directory.close();
        if (mounted && umount(mountPath.constData()) != 0) cleanup = errno;
        else mounted = false;
        if (ntfs.state() != QProcess::NotRunning) {
            if (!ntfs.waitForFinished(5000)) { ntfs.terminate(); if (!ntfs.waitForFinished(1000)) { ntfs.kill(); ntfs.waitForFinished(); } if (cleanup == 0) cleanup = ETIMEDOUT; }
            if ((ntfs.exitStatus() != QProcess::NormalExit || ntfs.exitCode() != 0) && cleanup == 0) cleanup = EIO;
        }
        if (!mounted && !mountPath.isEmpty() && rmdir(mountPath.constData()) != 0 && cleanup == 0) cleanup = errno;
        const int diskClosed = disk.close(), homeClosed = home.close(), socketClosed = channel.close();
        if (cleanup == 0) cleanup = diskClosed != 0 ? diskClosed : homeClosed != 0 ? homeClosed : socketClosed;
        if (cleanup != 0) error = cleanup;
    }
private:
    QString label_;
    int snapshot(UDisksObjects& objects, Device& current, bool mounts)
    {
        int error = ReadSaveDeviceSnapshot(QDBusConnection::systemBus(), request.source, objects, current);
        QStringList paths;
        if (error == 0) {
            const QString problem = mounts ? ListUDisksTargetMounts(objects, current, request.source.imageDevice, paths, request.source.allowFixedUsb, DeviceAccess::Read)
                : CheckUDisksTargetUse(objects, current, request.source.imageDevice, request.source.allowFixedUsb, DeviceAccess::Read);
            if (!problem.isEmpty()) { uprintfs(problem.toUtf8().constData()); error = EBUSY; }
        }
        return error;
    }
    int acquire(Descriptor& disk, Descriptor& home, Descriptor& directory, QByteArray& mountPath, bool& mounted, QProcess& ntfs)
    {
        int error = OpenNativeCallerHome(caller, home.fd);
        disk.fd = ::open(QFile::encodeName(request.source.path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (error == 0 && disk.fd < 0) error = errno;
        unsigned sector = 0;
        if (error == 0) error = CheckImageSaveDevice(disk.fd, request.source.device, request.source.size, request.source.diskSequence, sector);
        UDisksObjects objects;
        Device device;
        if (error == 0) error = snapshot(objects, device, true);
        QVariantMap volume;
        quint64 volumeNumber = 0;
        if (error == 0 && request.iso) {
            const QString block = QStringLiteral("org.freedesktop.UDisks2.Block");
            quint64 firstOffset = UINT64_MAX;
            for (auto item = objects.cbegin(); item != objects.cend(); ++item) {
                const QVariantMap partition = item.value().value(QStringLiteral("org.freedesktop.UDisks2.Partition"));
                const QVariantMap candidate = item.value().value(block);
                const bool whole = item.key().path() == request.source.objectPath;
                const bool member = qdbus_cast<QDBusObjectPath>(partition.value(QStringLiteral("Table"))).path() == request.source.objectPath;
                if ((!whole && !member) || candidate.value(QStringLiteral("IdUsage")).toString() != QLatin1String("filesystem")) continue;
                const quint64 offset = whole ? 0 : partition.value(QStringLiteral("Offset")).toULongLong();
                if (offset < firstOffset) { volume = candidate; firstOffset = offset; }
            }
            volumeNumber = volume.value(QStringLiteral("DeviceNumber")).toULongLong();
            if (volume.isEmpty() || volumeNumber == 0) error = ENOTSUP;
            label_ = volume.value(QStringLiteral("IdLabel")).toString();
            if (label_.isEmpty()) label_ = QStringLiteral("RUFUS");
        }
        NativeTargetMounts native;
        if (error == 0) error = CheckNativeTargetMounts(disk.fd, -1, native, home.fd);
        if (error == 0 && !MatchUDisksTargetMounts(objects, device, request.source.imageDevice,
            request.source.diskSequence, native, request.source.allowFixedUsb, DeviceAccess::Read).isEmpty()) error = ESTALE;
        if (error == 0) error = UnmountNativeTarget(disk.fd, -1, native, unmounted, home.fd);
        if (unmounted) uprintf("Unmounted %d source filesystem(s).", unmounted);
        // UDisks is asynchronous after a native unmount. Accept only a fresh,
        // matching empty mount snapshot, never hide a remount or service change.
        for (int attempt = 0; error == 0 && attempt < 200; attempt++) {
            const int current = snapshot(objects, device, false);
            if (current == 0) break;
            if (current != EBUSY) { error = current; break; }
            if (attempt == 199) { error = ETIMEDOUT; break; }
            QThread::msleep(pollMs);
        }
        if (error == 0) error = disk.close();
        if (error != 0) return error;
        disk.fd = ::open(QFile::encodeName(request.source.path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (request.iso || !request.source.lockDevice ? 0 : O_EXCL));
        if (disk.fd < 0) return errno;
        error = CheckImageSaveDevice(disk.fd, request.source.device, request.source.size, request.source.diskSequence, sector);
        NativeTargetMounts empty;
        if (error == 0) error = CheckNativeTargetMounts(disk.fd, -1, empty, home.fd);
        if (error == 0 && !empty.volumes.isEmpty()) error = EBUSY;
        if (error != 0 || !request.iso) return error;
        const QByteArray devicePath = volume.value(QStringLiteral("Device")).toByteArray().split('\0').first();
        struct stat node;
        if (stat(devicePath.constData(), &node) != 0) return errno;
        if (!S_ISBLK(node.st_mode) || quint64(node.st_rdev) != volumeNumber) return ESTALE;
        const QString member = QFileInfo(QFile::decodeName(sysfs(volumeNumber))).canonicalFilePath();
        const QString whole = QFileInfo(QFile::decodeName(sysfs(request.source.device))).canonicalFilePath();
        if (member != whole && QFileInfo(member).absolutePath() != whole) return ESTALE;
        const QString type = volume.value(QStringLiteral("IdType")).toString();
        if (type != QLatin1String("vfat") && type != QLatin1String("exfat") && type != QLatin1String("ntfs")
            && type != QLatin1String("ext2") && type != QLatin1String("ext3") && type != QLatin1String("ext4") && type != QLatin1String("udf")) return ENOTSUP;
        error = CreatePrivateMountNamespace();
        if (error != 0) return error;
        error = CheckNativeTargetMounts(disk.fd, -1, empty, home.fd);
        if (error != 0 || !empty.volumes.isEmpty()) return error != 0 ? error : EBUSY;
        QByteArray temporary("/run/rufus-save-XXXXXX");
        if (!mkdtemp(temporary.data())) return errno;
        mountPath = temporary;
        QByteArray options;
        if (type == QLatin1String("vfat") || type == QLatin1String("exfat")) options = "uid=" + QByteArray::number(caller) + ",fmask=0177,dmask=0077";
        if (type == QLatin1String("vfat")) options += ",utf8=1,shortname=mixed";
        if (type == QLatin1String("exfat")) options += ",iocharset=utf8";
        if (type.startsWith(QLatin1String("ext")) && type != QLatin1String("ext2")) options = "noload";
        if (type == QLatin1String("ntfs")) options = "iocharset=utf8,uid=" + QByteArray::number(caller);
        const unsigned long flags = MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC;
        utsname kernel = {};
        if (type == QLatin1String("ntfs") && uname(&kernel) != 0) return errno;
        const bool oldNtfs = type == QLatin1String("ntfs") && QVersionNumber::fromString(QString::fromLatin1(kernel.release)) < QVersionNumber(7, 1);
        int mountError = oldNtfs ? ENODEV : 0;
        if (!oldNtfs && mount(devicePath.constData(), mountPath.constData(), type.toLatin1().constData(), flags, options.constData()) != 0) mountError = errno;
        if (mountError != 0) {
            if (type != QLatin1String("ntfs") || mountError != ENODEV) return mountError;
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
            ntfs.setProcessEnvironment(environment);
            ntfs.setStandardInputFile(QProcess::nullDevice());
            ntfs.setStandardOutputFile(QProcess::nullDevice());
            ntfs.setProcessChannelMode(QProcess::ForwardedErrorChannel);
            ntfs.start(QStringLiteral(RUFUS_NTFS_3G_PATH), {QStringLiteral("-o"),
                QStringLiteral("ro,no_detach,no_def_opts,norecover,nodev,nosuid,noexec,allow_other,umask=0077,uid=%1").arg(caller),
                QFile::decodeName(devicePath), QFile::decodeName(mountPath)});
            if (!ntfs.waitForStarted(5000)) return EIO;
        } else mounted = true;
        for (int attempt = 0; attempt < 200; attempt++) {
            directory.fd = ::open(mountPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            struct stat info;
            struct statvfs filesystem;
            if (directory.fd >= 0 && fstat(directory.fd, &info) == 0 && quint64(info.st_dev) == volumeNumber
                && fstatvfs(directory.fd, &filesystem) == 0 && (filesystem.f_flag & ST_RDONLY)) { mounted = true; return 0; }
            directory.close();
            if (mounted || IsRufusJobCancelled()) return IsRufusJobCancelled() ? ECANCELED : ESTALE;
            if (ntfs.state() == QProcess::NotRunning) return EIO;
            QThread::msleep(pollMs);
        }
        return ETIMEDOUT;
    }
};
class SourceServer final : public QObject {
    Q_OBJECT
public:
    explicit SourceServer(uid_t caller) : notifier_(STDIN_FILENO, QSocketNotifier::Read)
    {
        worker_.caller = caller;
        connect(&notifier_, &QSocketNotifier::activated, this, &SourceServer::input);
        connect(&worker_, &QThread::finished, this, &SourceServer::finished);
    }
    ~SourceServer() { worker_.requestInterruption(); worker_.release.store(true); worker_.wait(); }
private slots:
    void input()
    {
        char data[messageBytes];
        const ssize_t count = read(STDIN_FILENO, data, sizeof(data));
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) return;
        if (count <= 0) { stop(true); return; }
        buffer_.append(data, count);
        if (buffer_.size() > messageBytes) { stop(true); return; }
        const int newline = buffer_.indexOf('\n');
        if (newline < 0) return;
        const QByteArray line = buffer_.left(newline); buffer_.remove(0, newline + 1);
        if (!started_) {
            if (!DecodeSaveSourceRequest(QJsonDocument::fromJson(line).object(), worker_.request)) { QCoreApplication::exit(77); return; }
            started_ = true; worker_.start();
        } else if (line == "finish") stop(false);
        else stop(true);
        if (!buffer_.isEmpty()) inputPending();
    }
    void finished()
    {
        worker_.wait();
        const QByteArray result = QJsonDocument(QJsonObject{{"event", "result"}, {"error", worker_.error},
            {"unmounted", worker_.unmounted}}).toJson(QJsonDocument::Compact) + '\n';
        if (write(STDOUT_FILENO, result.constData(), size_t(result.size())) != result.size()) QCoreApplication::exit(1);
        else QCoreApplication::exit(worker_.error == 0 ? 0 : 1);
    }
    void inputPending()
    {
        // A cancellation may follow the request in the same pipe read.
        if (started_ && buffer_.contains('\n')) {
            const QByteArray command = buffer_.trimmed(); buffer_.clear();
            stop(command != "finish");
        }
    }
private:
    void stop(bool cancel)
    {
        notifier_.setEnabled(false);
        if (!started_) { QCoreApplication::exit(77); return; }
        if (cancel) worker_.requestInterruption();
        worker_.release.store(true);
    }
    QSocketNotifier notifier_;
    SourceThread worker_;
    QByteArray buffer_;
    bool started_ = false;
};
}
static QJsonObject encodeSaveSourceContent(const SaveSourceRequest& r)
{
    return {{"protocol", 1}, {"path", r.source.path}, {"device", QString::number(r.source.device)},
        {"size", QString::number(r.source.size)}, {"sequence", QString::number(r.source.diskSequence)},
        {"object", r.source.objectPath}, {"owner", r.source.serviceOwner}, {"identity", QJsonArray::fromStringList(r.source.identity)},
        {"outputDevice", QString::number(r.source.imageDevice)}, {"allowFixedUsb", r.source.allowFixedUsb},
        {"iso", r.iso}, {"socket", r.socket}, {"token", QString::fromLatin1(r.token)}};
}
static bool decodeSaveSourceContent(const QJsonObject& object, SaveSourceRequest& r)
{
    if (object.size() != 13 || object.value(QStringLiteral("protocol")).toInt() != 1) return false;
    bool valid = true;
    r.source.device = number(object, "device", valid); r.source.size = number(object, "size", valid);
    r.source.diskSequence = number(object, "sequence", valid); r.source.imageDevice = number(object, "outputDevice", valid);
    r.source.path = object.value(QStringLiteral("path")).toString(); r.source.objectPath = object.value(QStringLiteral("object")).toString();
    r.source.serviceOwner = object.value(QStringLiteral("owner")).toString();
    r.source.identity.clear();
    for (const QJsonValue& item : object.value(QStringLiteral("identity")).toArray()) {
        if (!item.isString()) return false;
        r.source.identity.append(item.toString());
    }
    if (!object.value(QStringLiteral("iso")).isBool() || !object.value(QStringLiteral("allowFixedUsb")).isBool()) return false;
    r.iso = object.value(QStringLiteral("iso")).toBool(); r.source.allowFixedUsb = object.value(QStringLiteral("allowFixedUsb")).toBool();
    r.socket = object.value(QStringLiteral("socket")).toString(); r.token = object.value(QStringLiteral("token")).toString().toLatin1();
    r.source.isDevice = r.source.rawWrite = true;
    const bool bounded = r.source.path.startsWith(QLatin1String("/dev/")) && !r.source.path.contains(QChar(0))
        && r.source.path.size() < 4096 && r.source.objectPath.startsWith(QLatin1String("/org/freedesktop/UDisks2/"))
        && r.source.serviceOwner.startsWith(QLatin1Char(':')) && !r.source.identity.isEmpty()
        && r.source.device > 0 && r.source.size > 0 && r.source.size <= (quint64(64) << 40) && r.source.diskSequence > 0;
    return valid && bounded && r.socket.startsWith(QLatin1Char('/')) && !r.socket.contains(QChar(0))
        && QFile::encodeName(r.socket).size() < 108 && r.token.size() == 64 && QByteArray::fromHex(r.token).toHex() == r.token;
}
QJsonObject EncodeSaveSourceRequest(const SaveSourceRequest& request)
{
    if (!request.source.exclusions.valid() || !ValidDeviceClass(request.source.deviceClass) || request.source.deviceClass == DeviceClass::Optical) return {};
    QJsonObject object = encodeSaveSourceContent(request);
    if (request.source.imageInode != 0) object.insert("outputInode", QString::number(request.source.imageInode));
    QJsonArray usb;
    for (quint32 id : request.source.exclusions.usb) usb.append(QString::number(id));
    if (request.source.deviceClass != DeviceClass::Usb || !request.source.lockDevice || !request.source.exclusions.empty()) object.insert("nativePolicy", QJsonObject{{"class", int(DeviceClassMask(request.source.deviceClass))},
        {"lock", request.source.lockDevice}, {"usb", usb}, {"disks", QJsonArray::fromStringList(request.source.exclusions.disks)}});
    return object;
}
bool DecodeSaveSourceRequest(const QJsonObject& object, SaveSourceRequest& request)
{
    QJsonObject content = object;
    quint64 outputInode = 0;
    if (content.contains("outputInode")) {
        const QJsonValue value = content.take("outputInode");
        bool valid = false; outputInode = value.toString().toULongLong(&valid);
        if (!value.isString() || !valid || outputInode == 0 || QString::number(outputInode) != value.toString()) return false;
    }
    DeviceExclusions exclusions;
    DeviceClass kind = DeviceClass::Usb;
    bool lockDevice = true;
    if (content.contains("nativePolicy")) {
        const QJsonValue value = content.take("nativePolicy");
        if (!value.isObject()) return false;
        const auto policy = value.toObject();
        if (policy.size() != 4 || !policy.value("class").isDouble() || !policy.value("lock").isBool()
            || !policy.value("usb").isArray() || !policy.value("disks").isArray()) return false;
        const double number = policy.value("class").toDouble();
        if (number < 1 || number > AllDeviceClasses || number != double(int(number))) return false;
        kind = DeviceClass(quint32(number));
        if (!ValidDeviceClass(kind) || kind == DeviceClass::Optical) return false;
        lockDevice = policy.value("lock").toBool();
        for (const auto id : policy.value("usb").toArray()) {
            if (!id.isString()) return false;
            bool valid = false; const quint64 number = id.toString().toULongLong(&valid);
            if (!valid || number > UINT32_MAX || QString::number(number) != id.toString()) return false;
            exclusions.usb.append(quint32(number));
        }
        for (const auto id : policy.value("disks").toArray()) {
            if (!id.isString()) return false;
            exclusions.disks.append(id.toString());
        }
        if (!exclusions.valid()) return false;
    }
    if (!decodeSaveSourceContent(content, request)) return false;
    request.source.imageInode = outputInode;
    request.source.deviceClass = kind; request.source.lockDevice = lockDevice; request.source.exclusions = exclusions;
    return true;
}
int SendSaveSourceDescriptor(int socket, int descriptor, const QJsonObject& metadata)
{
    const QByteArray bytes = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    if (bytes.size() > messageBytes) return E2BIG;
    iovec vector{const_cast<char*>(bytes.constData()), size_t(bytes.size())};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message = {}; message.msg_iov = &vector; message.msg_iovlen = 1;
    if (descriptor >= 0) {
        message.msg_control = control; message.msg_controllen = sizeof(control);
        cmsghdr* header = CMSG_FIRSTHDR(&message); header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(header), &descriptor, sizeof(int));
    }
    return sendmsg(socket, &message, MSG_NOSIGNAL) == bytes.size() ? 0 : errno ? errno : EIO;
}
int ReceiveSaveSourceDescriptor(int socket, int& descriptor, QJsonObject& metadata)
{
    descriptor = -1;
    char bytes[messageBytes]; alignas(cmsghdr) char control[CMSG_SPACE(8 * sizeof(int))] = {};
    iovec vector{bytes, sizeof(bytes)}; msghdr message = {};
    message.msg_iov = &vector; message.msg_iovlen = 1; message.msg_control = control; message.msg_controllen = sizeof(control);
    const ssize_t count = recvmsg(socket, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (count < 0) return errno;
    int descriptors = 0;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) continue;
        const size_t size = header->cmsg_len - CMSG_LEN(0);
        for (size_t offset = 0; offset + sizeof(int) <= size; offset += sizeof(int)) {
            int fd; memcpy(&fd, CMSG_DATA(header) + offset, sizeof(fd));
            if (++descriptors == 1) descriptor = fd; else close(fd);
        }
    }
    metadata = QJsonDocument::fromJson(QByteArray(bytes, int(count))).object();
    const bool valid = count > 0 && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) && descriptors <= 1 && !metadata.isEmpty();
    if (!valid && descriptor >= 0) { close(descriptor); descriptor = -1; }
    return valid ? 0 : EPROTO;
}
int RunSaveSourceHelper(int argc, char** argv)
{
    bool valid = false;
    const quint64 caller = qEnvironmentVariable("PKEXEC_UID").toULongLong(&valid);
    if (getuid() != 0 || geteuid() != 0 || !valid || caller == 0 || caller >= UINT32_MAX) return 77;
    struct stat input, output;
    if (fstat(STDIN_FILENO, &input) != 0 || fstat(STDOUT_FILENO, &output) != 0 || !S_ISFIFO(input.st_mode) || !S_ISFIFO(output.st_mode)) return 77;
#ifndef RUFUS_SAVE_GUEST_TEST
    qunsetenv("DBUS_SYSTEM_BUS_ADDRESS"); qunsetenv("DBUS_SESSION_BUS_ADDRESS");
#endif
    signal(SIGPIPE, SIG_IGN);
    if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) != 0) return 77;
    QCoreApplication app(argc, argv);
    SourceServer server{uid_t(caller)};
    return app.exec();
}
}
#include "save_source.moc"
