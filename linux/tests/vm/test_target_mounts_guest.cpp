// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/target_mounts.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSemaphore>
#include <QThread>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <libmount/libmount.h>
#include <linux/magic.h>
#include <sched.h>
#include <spawn.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/swap.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace
{
bool cancelled = false;
int unmountCalls = 0;
enum class Fault { None, Fail, CancelAfterFirst };
Fault fault = Fault::None;
enum class MountInfo { Native, Missing, Malformed, Unsupported, SourceOnly };
MountInfo mountInfo = MountInfo::Native;
enum class NamespaceFault { None, Unshare, Propagation };
NamespaceFault namespaceFault = NamespaceFault::None;
constexpr int namespaceTimeoutMs = 10000;
constexpr int diskBytes = 64 * 1024 * 1024;

void require(bool valid, const char* message)
{
    if (!valid)
        qFatal("Native mount guest: %s (errno=%d)", message, errno);
}

QByteArray readFile(const char* path)
{
    QFile file(QString::fromLocal8Bit(path));
    require(file.open(QIODevice::ReadOnly), "read fixture identity");
    return file.readAll();
}

void runTool(const QString& program, const QStringList& arguments)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.start(program, arguments);
    require(process.waitForFinished(10000), "fixture tool completion");
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, "fixture tool result");
}

void mountFirst()
{
    require(mount("/dev/sda1", "/mnt/usb one", "vfat", MS_NOEXEC | MS_NOSUID | MS_NODEV, "") == 0, "mount first");
}

void mountSecond()
{
    require(mount("/dev/sda2", "/mnt/usb two", "vfat", MS_NOEXEC | MS_NOSUID | MS_NODEV, "") == 0, "mount second");
}

rufus::NativeTargetMounts inspect(int disk, int count)
{
    rufus::NativeTargetMounts result;
    const int error = rufus::CheckNativeTargetMounts(disk, -1, result);
    require(error == 0 && result.volumes.size() == count, "native mount inventory");
    require(result.device != 0 && result.diskSequence != 0, "native disk identity");
    return result;
}

void refusal(int disk, int source, int expected, const char* name)
{
    rufus::NativeTargetMounts result;
    result.device = 123;
    const int error = rufus::CheckNativeTargetMounts(disk, source, result);
    require(error == expected, name);
    require(result.device == 0 && result.volumes.isEmpty(), "refusal clears result");
    printf("RUFUS_MOUNTS_CASE %s ERROR=%d\n", name, error);
}

void claimFree()
{
    const int disk = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(disk >= 0, "exclusive claim after unmount");
    close(disk);
}

void callerAccounts()
{
    struct AccountCase { uid_t uid; int error; const char* name; };
    const AccountCase cases[] = {
        {0, EINVAL, "home-root-uid"}, {uid_t(-1), EINVAL, "home-invalid-uid"},
        {2000, ENOENT, "home-unknown-uid"}, {1002, EINVAL, "home-relative"},
        {1003, ENOENT, "home-missing"}, {1004, ENOTDIR, "home-file"}};
    for (const AccountCase& test : cases)
    {
        int home = -1;
        require(rufus::OpenNativeCallerHome(test.uid, home) == test.error, test.name);
        require(home == -1, "failed account lookup leaves no descriptor");
        printf("RUFUS_MOUNTS_CASE %s ERROR=%d\n", test.name, test.error);
    }
    cancelled = true;
    int home = -1;
    require(rufus::OpenNativeCallerHome(1000, home) == ECANCELED && home == -1, "cancelled home lookup");
    cancelled = false;
    printf("RUFUS_MOUNTS_CASE home-cancel ERROR=%d\n", ECANCELED);
}

void callerHomeRefusals(int disk)
{
    struct HomeCase { uid_t uid; int error; const char* name; };
    const HomeCase cases[] = {{1001, EBUSY, "home-on-target"}, {1005, ENOTSUP, "home-unresolved"}};
    const auto plan = inspect(disk, 1);
    for (const HomeCase& test : cases)
    {
        int home = -1;
        require(rufus::OpenNativeCallerHome(test.uid, home) == 0, "retain caller home");
        rufus::NativeTargetMounts result;
        result.device = 123;
        require(rufus::CheckNativeTargetMounts(disk, -1, result, home) == test.error, test.name);
        require(result.device == 0 && result.volumes.isEmpty(), "home refusal clears result");
        const int before = unmountCalls;
        int unmounted = -1;
        require(rufus::UnmountNativeTarget(disk, -1, plan, unmounted, home) == test.error, "home rechecked before unmount");
        require(unmounted == 0 && unmountCalls == before, "home refusal never attempts unmount");
        close(home);
        printf("RUFUS_MOUNTS_CASE %s ERROR=%d\n", test.name, test.error);
    }
    rufus::NativeTargetMounts result;
    require(rufus::CheckNativeTargetMounts(disk, -1, result, -2) == EBADF, "invalid home descriptor");
    printf("RUFUS_MOUNTS_CASE home-invalid-fd ERROR=%d\n", EBADF);
    const int file = open("/etc/machine-id", O_PATH | O_CLOEXEC);
    require(file >= 0, "home type fixture");
    require(rufus::CheckNativeTargetMounts(disk, -1, result, file) == EINVAL, "home must be a directory");
    close(file);
    printf("RUFUS_MOUNTS_CASE home-wrong-type ERROR=%d\n", EINVAL);
}

int retainedCallerHome(int disk)
{
    require(qputenv("HOME", "/mnt/usb one"), "untrusted home environment fixture");
    int home = -1;
    require(rufus::OpenNativeCallerHome(1000, home) == 0, "account home overrides environment");
    require((fcntl(home, F_GETFD) & FD_CLOEXEC) != 0, "home does not leak to children");
    require(unlink("/home/caller") == 0 && symlink("/mnt/usb one", "/home/caller") == 0, "retarget home symlink");
    rufus::NativeTargetMounts plan;
    require(rufus::CheckNativeTargetMounts(disk, -1, plan, home) == 0, "retained home keeps original filesystem");
    int changed = -1;
    require(rufus::OpenNativeCallerHome(1000, changed) == 0, "open retargeted home");
    require(rufus::CheckNativeTargetMounts(disk, -1, plan, changed) == EBUSY, "new home resolves target filesystem");
    close(changed);
    require(unlink("/home/caller") == 0 && symlink("/run/source", "/home/caller") == 0, "restore home symlink");
    printf("RUFUS_MOUNTS_CASE home-symlink-retained ERROR=0\n");
    int large = -1;
    require(rufus::OpenNativeCallerHome(1006, large) == 0, "account buffer grows on ERANGE");
    require(rufus::CheckNativeTargetMounts(disk, -1, plan, large) == 0, "large account retained home");
    close(large);
    printf("RUFUS_MOUNTS_CASE home-account-buffer ERROR=0\n");
    return home;
}

quint64 mountNamespace(const char* path)
{
    struct stat info;
    require(stat(path, &info) == 0, "namespace identity");
    return quint64(info.st_ino);
}

class NamespaceWorker final : public QThread
{
public:
    explicit NamespaceWorker(int disk, bool cancel) : disk_(disk), cancel_(cancel) {}
    QSemaphore mounted;
    QSemaphore release;
    int error = 0;
    bool cleaned = false;

protected:
    void run() override
    {
        const quint64 original = mountNamespace("/proc/thread-self/ns/mnt");
        if (cancel_)
            requestInterruption();
        error = rufus::CreatePrivateMountNamespace();
        if (error != 0)
        {
            mounted.release();
            return;
        }
        const quint64 isolated = mountNamespace("/proc/thread-self/ns/mnt");
        require(isolated != original, "worker got its own namespace");
        require(mountNamespace("/proc/self/ns/mnt") == original, "main thread kept original namespace");
        mountFirst();
        auto plan = inspect(disk_, 1);
        // Verify both child launch mechanisms used by the production worker.
        const QString program = QCoreApplication::applicationFilePath();
        const QString expected = QString::number(isolated);
        runTool(program, {QStringLiteral("--namespace-child"), expected});
        QByteArray executable = QFile::encodeName(program);
        QByteArray argument = expected.toLatin1();
        char option[] = "--namespace-child";
        char* arguments[] = {executable.data(), option, argument.data(), nullptr};
        pid_t child;
        require(posix_spawn(&child, executable.constData(), nullptr, nullptr, arguments, environ) == 0, "spawn in worker namespace");
        int status;
        require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "spawn child completed");
        mounted.release();
        require(release.tryAcquire(1, namespaceTimeoutMs), "release namespace worker");
        int unmounted = 0;
        error = rufus::UnmountNativeTarget(disk_, -1, plan, unmounted);
        require(error == 0 && unmounted == 1, "worker unmounts its own view");
        inspect(disk_, 0);
        cleaned = true;
    }

private:
    const int disk_;
    const bool cancel_;
};

void threadNamespaces(int disk)
{
    const quint64 original = mountNamespace("/proc/thread-self/ns/mnt");
    // Shared propagation makes a missing MS_PRIVATE step observable by the main
    // thread. This changes only the fixture VM's namespace.
    require(mount(nullptr, "/", nullptr, MS_REC | MS_SHARED, nullptr) == 0, "shared fixture root");
    for (NamespaceFault injected : {NamespaceFault::Unshare, NamespaceFault::Propagation, NamespaceFault::None})
    {
        namespaceFault = injected;
        NamespaceWorker worker(disk, false);
        worker.start();
        require(worker.mounted.tryAcquire(1, namespaceTimeoutMs), "namespace worker ready");
        inspect(disk, 0); // /proc/self sees this main thread; thread-self sees the worker.
        require(mountNamespace("/proc/thread-self/ns/mnt") == original, "main namespace unchanged");
        if (injected == NamespaceFault::None)
        {
            const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
            require(claim < 0 && errno == EBUSY, "worker mount holds device");
            worker.release.release();
        }
        require(worker.wait(namespaceTimeoutMs), "namespace worker stopped");
        const int expected = injected == NamespaceFault::Unshare ? EPERM : injected == NamespaceFault::Propagation ? EIO : 0;
        require(worker.error == expected, "namespace failure propagated");
        require(worker.cleaned == (injected == NamespaceFault::None), "namespace cleanup completion");
        claimFree();
        const char* name = injected == NamespaceFault::Unshare ? "worker-unshare-failure" :
            injected == NamespaceFault::Propagation ? "worker-propagation-failure" : "worker-namespace-children";
        printf("RUFUS_MOUNTS_CASE %s ERROR=%d\n", name, expected);
    }
    NamespaceWorker cancelledWorker(disk, true);
    cancelledWorker.start();
    require(cancelledWorker.wait(namespaceTimeoutMs), "cancelled namespace worker stopped");
    require(cancelledWorker.error == ECANCELED, "cancelled isolation refused");
    require(mountNamespace("/proc/thread-self/ns/mnt") == original, "cancelled worker left main namespace alone");
    claimFree();
    printf("RUFUS_MOUNTS_CASE worker-cancel-before ERROR=%d\n", ECANCELED);
    require(mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == 0, "restore private fixture root");
}

void startFuse(QProcess& daemon, const QString& driver)
{
    daemon.setProcessChannelMode(QProcess::ForwardedChannels);
    daemon.setStandardInputFile(QProcess::nullDevice());
    daemon.start(QStringLiteral("/usr/bin/") + driver, {QStringLiteral("/dev/sda1"), QStringLiteral("/mnt/usb one"),
        QStringLiteral("-o"), QStringLiteral("rw,no_detach,no_def_opts,norecover,nodev,nosuid,noexec")});
    require(daemon.waitForStarted(namespaceTimeoutMs), "start foreground NTFS-3G");
    struct stat partition;
    require(stat("/dev/sda1", &partition) == 0, "FUSE backing partition");
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < namespaceTimeoutMs)
    {
        struct stat info;
        struct statfs filesystem;
        const bool mounted = stat("/mnt/usb one", &info) == 0 && info.st_dev == partition.st_rdev;
        if (mounted)
        {
            require(statfs("/mnt/usb one", &filesystem) == 0 && filesystem.f_type == FUSE_SUPER_MAGIC,
                "real FUSE filesystem with block dev_t");
            require(readFile("/proc/thread-self/mountinfo").contains(" - fuseblk "), "kernel reports fuseblk");
            return;
        }
        require(!daemon.waitForFinished(25), "NTFS-3G stays alive while mounting");
    }
    require(false, "NTFS-3G mount deadline");
}

void fuseRefusals(int disk, const QByteArray& driver, const rufus::NativeTargetMounts& plan)
{
    const int source = open("/mnt/usb one/marker", O_RDONLY | O_CLOEXEC);
    require(source >= 0, "retained FUSE source");
    refusal(disk, source, EBUSY, (driver + "-source").constData());
    int unmounted = -1;
    require(rufus::UnmountNativeTarget(disk, -1, plan, unmounted) == EBUSY && unmounted == 0,
        "ordinary FUSE unmount refuses an open file");
    close(source);
    printf("RUFUS_MOUNTS_CASE %s-busy ERROR=%d\n", driver.constData(), EBUSY);
    const int home = open("/mnt/usb one", O_PATH | O_DIRECTORY | O_CLOEXEC);
    require(home >= 0, "retained FUSE home");
    const int beforeHome = unmountCalls;
    require(rufus::UnmountNativeTarget(disk, -1, plan, unmounted, home) == EBUSY && unmounted == 0,
        "FUSE caller home refused");
    require(unmountCalls == beforeHome, "home check precedes unmount syscall");
    close(home);
    printf("RUFUS_MOUNTS_CASE %s-home ERROR=%d\n", driver.constData(), EBUSY);

    require(mount("/mnt/usb one", "/mnt/alias", nullptr, MS_BIND, nullptr) == 0, "FUSE bind alias");
    refusal(disk, -1, EBUSY, (driver + "-alias").constData());
    require(umount("/mnt/alias") == 0, "FUSE alias cleanup");
    require(mount("tmpfs", "/mnt/usb one/nested", "tmpfs", 0, "size=1m") == 0, "FUSE nested mount");
    refusal(disk, -1, EBUSY, (driver + "-nested").constData());
    require(umount("/mnt/usb one/nested") == 0, "FUSE nested cleanup");
    require(mount("/mnt/usb one", "/home", nullptr, MS_BIND, nullptr) == 0, "FUSE system alias");
    refusal(disk, -1, EBUSY, (driver + "-system").constData());
    require(umount("/home") == 0, "FUSE system alias cleanup");

    auto stale = plan;
    stale.volumes.first().id++;
    const int beforeStale = unmountCalls;
    require(rufus::UnmountNativeTarget(disk, -1, stale, unmounted) == ESTALE && unmounted == 0, "FUSE stale mount id");
    cancelled = true;
    require(rufus::UnmountNativeTarget(disk, -1, plan, unmounted) == ECANCELED && unmounted == 0, "FUSE cancellation");
    cancelled = false;
    require(unmountCalls == beforeStale, "stale/cancelled FUSE plans never unmount");
    printf("RUFUS_MOUNTS_CASE %s-stale ERROR=%d\n", driver.constData(), ESTALE);
    printf("RUFUS_MOUNTS_CASE %s-cancel ERROR=%d\n", driver.constData(), ECANCELED);
}

void fuseMounts(int disk)
{
    for (const char* driver : {"ntfs-3g", "lowntfs-3g"})
    {
        runTool(QStringLiteral("/usr/bin/mkntfs"), {QStringLiteral("-F"), QStringLiteral("-Q"), QStringLiteral("/dev/sda1")});
        QProcess daemon;
        startFuse(daemon, QString::fromLatin1(driver));
        const auto plan = inspect(disk, 1);
        struct stat partition;
        require(stat("/dev/sda1", &partition) == 0, "FUSE partition identity");
        require(plan.volumes.first().device == quint64(partition.st_rdev), "native inventory retains real block identity");
        const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        require(claim < 0 && errno == EBUSY, "FUSE mount holds exclusive block claim");
        require(QDir().mkpath(QStringLiteral("/mnt/usb one/nested")), "FUSE nested fixture directory");
        const QByteArray marker(4096, 'n');
        {
            QFile file(QStringLiteral("/mnt/usb one/marker"));
            require(file.open(QIODevice::WriteOnly) && file.write(marker) == marker.size(), "FUSE write fixture");
        }
        fuseRefusals(disk, driver, plan);
        int unmounted = -1;
        require(rufus::UnmountNativeTarget(disk, -1, plan, unmounted) == 0 && unmounted == 1, "normal FUSE unmount");
        claimFree(); // Do not wait for or terminate the daemon to make acquisition pass.
        require(daemon.waitForFinished(namespaceTimeoutMs), "FUSE daemon exits after unmount");
        require(daemon.exitStatus() == QProcess::NormalExit && daemon.exitCode() == 0, "FUSE clean daemon exit");
        inspect(disk, 0);
        startFuse(daemon, QString::fromLatin1(driver));
        require(readFile("/mnt/usb one/marker") == marker, "unmount flushed NTFS marker");
        const auto remounted = inspect(disk, 1);
        require(rufus::UnmountNativeTarget(disk, -1, remounted, unmounted) == 0 && unmounted == 1, "FUSE readback cleanup");
        require(daemon.waitForFinished(namespaceTimeoutMs) && daemon.exitCode() == 0, "FUSE readback daemon exit");
        claimFree();
        printf("RUFUS_MOUNTS_CASE %s-success ERROR=0\n", driver);
    }
}

}

extern "C" int IsRufusJobCancelled()
{
    return cancelled || QThread::currentThread()->isInterruptionRequested();
}
extern "C" int __real_unshare(int);
extern "C" int __wrap_unshare(int flags)
{
    if (namespaceFault == NamespaceFault::Unshare)
    {
        errno = EPERM;
        return -1;
    }
    return __real_unshare(flags);
}
extern "C" int __real_mount(const char*, const char*, const char*, unsigned long, const void*);
extern "C" int __wrap_mount(const char* source, const char* target, const char* type, unsigned long flags, const void* data)
{
    if (namespaceFault == NamespaceFault::Propagation && flags == (MS_REC | MS_PRIVATE))
    {
        errno = EIO;
        return -1;
    }
    return __real_mount(source, target, type, flags, data);
}
extern "C" int __real_mnt_table_parse_file(libmnt_table*, const char*);
extern "C" int __wrap_mnt_table_parse_file(libmnt_table* table, const char* filename)
{
    if (strcmp(filename, "/proc/thread-self/mountinfo") != 0 || mountInfo == MountInfo::Native)
        return __real_mnt_table_parse_file(table, filename);
    if (mountInfo == MountInfo::Missing)
        return -EACCES;
    QByteArray text = readFile(filename);
    if (mountInfo == MountInfo::Malformed)
        text.append("this is not mountinfo\n");
    else if (mountInfo == MountInfo::Unsupported)
        text.replace(" - vfat ", " - fuse ");
    else
    {
        QList<QByteArray> lines = text.split('\n');
        for (QByteArray& line : lines)
        {
            if (!line.contains(" - vfat ")) continue;
            QList<QByteArray> fields = line.split(' ');
            require(fields.size() > 6, "native mountinfo fields");
            fields[2] = "0:12345"; // Source still names sda1, but kernel identity is anonymous.
            line = fields.join(' ');
            line.replace(" - vfat ", " - fuseblk ");
        }
        text = lines.join('\n');
    }
    FILE* stream = fmemopen(text.data(), size_t(text.size()), "r");
    require(stream != nullptr, "mountinfo fault stream");
    const int result = mnt_table_parse_stream(table, stream, filename);
    fclose(stream);
    return result;
}
extern "C" int __real_umount2(const char*, int);
extern "C" int __wrap_umount2(const char* path, int flags)
{
    unmountCalls++;
    require(flags == UMOUNT_NOFOLLOW, "no force/lazy flags");
    require(strncmp(path, "/proc/self/fd/", strlen("/proc/self/fd/")) == 0, "retained parent path");
    if (fault == Fault::Fail)
    {
        errno = EIO;
        return -1;
    }
    const int result = __real_umount2(path, flags);
    if (fault == Fault::CancelAfterFirst && result == 0)
        cancelled = true;
    return result;
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    require(getuid() == 0 && geteuid() == 0, "guest root only");
    require(readFile("/sys/class/dmi/id/product_name").trimmed() == "RufusMountFixture", "fixture VM only");
    require(readFile("/proc/cmdline").contains("rufus.mount_fixture=1"), "fixture boot flag");
    require(readFile("/sys/block/sda/device/model").startsWith("QEMU HARDDISK"), "QEMU disk only");
    require(readFile("/sys/block/sda/size").trimmed().toULongLong() == diskBytes / 512, "fixture capacity");
    if (argc == 3 && strcmp(argv[1], "--namespace-child") == 0)
    {
        require(mountNamespace("/proc/thread-self/ns/mnt") == QByteArray(argv[2]).toULongLong(), "child inherited worker namespace");
        const int disk = open("/dev/sda", O_RDONLY | O_CLOEXEC);
        require(disk >= 0, "child read-only descriptor");
        inspect(disk, 1);
        close(disk);
        return 0;
    }
    const int disk = open("/dev/sda", O_RDONLY | O_CLOEXEC);
    require(disk >= 0, "retained read-only disk");
    inspect(disk, 0);
    claimFree();
    printf("RUFUS_MOUNTS_CASE unmounted ERROR=0\n");
    refusal(-1, -1, EBADF, "invalid-disk");
    const int regular = open("/etc/machine-id", O_RDONLY | O_CLOEXEC);
    require(regular >= 0, "regular descriptor");
    refusal(regular, -1, ENOTBLK, "regular-disk");
    close(regular);
    const int partition = open("/dev/sda1", O_RDONLY | O_CLOEXEC);
    require(partition >= 0, "partition descriptor");
    refusal(partition, -1, ENOTBLK, "partition-disk");
    close(partition);
    cancelled = true;
    refusal(disk, -1, ECANCELED, "cancel-before");
    cancelled = false;

    callerAccounts();
    mountFirst();
    callerHomeRefusals(disk);
    auto plan = inspect(disk, 1);
    require(plan.volumes.first().path == "/mnt/usb one", "libmount decoded escaped space");
    mountInfo = MountInfo::Missing;
    refusal(disk, -1, EACCES, "missing-mountinfo");
    mountInfo = MountInfo::Malformed;
    refusal(disk, -1, EBADMSG, "malformed-mountinfo");
    mountInfo = MountInfo::Unsupported;
    refusal(disk, -1, ENOTSUP, "unsupported-filesystem");
    mountInfo = MountInfo::SourceOnly;
    refusal(disk, -1, ENOTSUP, "fuse-source-only");
    mountInfo = MountInfo::Native;
    const int unresolved = open("/etc/machine-id", O_RDONLY | O_CLOEXEC);
    require(unresolved >= 0, "rootfs source");
    refusal(disk, unresolved, ENOTSUP, "unresolved-source");
    close(unresolved);
    const int source = open("/mnt/usb one/source.iso", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    require(source >= 0, "source fixture");
    refusal(disk, source, EBUSY, "source-on-target");
    int unmounted = -1;
    require(rufus::UnmountNativeTarget(disk, -1, plan, unmounted) == EBUSY && unmounted == 0, "open file is busy");
    inspect(disk, 1);
    printf("RUFUS_MOUNTS_CASE busy ERROR=%d\n", EBUSY);
    close(source);
    require(unlink("/mnt/usb one/source.iso") == 0, "remove source fixture");
    require(chmod("/mnt", 0777) == 0, "untrusted parent fixture");
    const int beforeParent = unmountCalls;
    require(rufus::UnmountNativeTarget(disk, -1, plan, unmounted) == EPERM && unmounted == 0, "untrusted mount parent");
    require(unmountCalls == beforeParent, "untrusted parent never unmounts");
    require(chmod("/mnt", 0755) == 0, "restore parent ownership");
    printf("RUFUS_MOUNTS_CASE untrusted-parent ERROR=%d\n", EPERM);

    require(mount("tmpfs", "/mnt/usb one/nested", "tmpfs", 0, "size=1m") == 0, "nested mount");
    refusal(disk, -1, EBUSY, "nested-mount");
    require(umount("/mnt/usb one/nested") == 0, "nested cleanup");
    require(mount("/mnt/usb one", "/mnt/alias", nullptr, MS_BIND, nullptr) == 0, "bind alias");
    refusal(disk, -1, EBUSY, "bind-alias");
    require(umount("/mnt/alias") == 0, "alias cleanup");
    require(mount("/mnt/usb one", "/home", nullptr, MS_BIND, nullptr) == 0, "system alias");
    refusal(disk, -1, EBUSY, "system-alias");
    require(umount("/home") == 0, "system alias cleanup");
    require(chdir("/mnt/usb one") == 0, "target current directory");
    refusal(disk, -1, EBUSY, "current-directory");
    require(chdir("/") == 0, "leave current directory");

    const int beforeStale = unmountCalls;
    auto stale = plan;
    stale.diskSequence++;
    require(rufus::UnmountNativeTarget(disk, -1, stale, unmounted) == ESTALE && unmounted == 0, "stale disk sequence");
    stale = plan;
    stale.volumes.first().id++;
    require(rufus::UnmountNativeTarget(disk, -1, stale, unmounted) == ESTALE && unmounted == 0, "stale mount id");
    require(unmountCalls == beforeStale, "stale plans never unmount");
    printf("RUFUS_MOUNTS_CASE stale ERROR=%d\n", ESTALE);
    fault = Fault::Fail;
    require(rufus::UnmountNativeTarget(disk, -1, plan, unmounted) == EIO && unmounted == 0, "syscall failure");
    fault = Fault::None;
    inspect(disk, 1);
    printf("RUFUS_MOUNTS_CASE syscall-failure ERROR=%d\n", EIO);

    // A private copy keeps the kernel mount alive after the original is gone.
    // Reproduce the helper/UDisks namespace issue without touching host mounts.
    int ready[2], finish[2];
    require(pipe(ready) == 0 && pipe(finish) == 0, "namespace synchronization");
    const pid_t child = fork();
    require(child >= 0, "namespace child");
    if (child == 0)
    {
        close(ready[0]); close(finish[1]);
        require(unshare(CLONE_NEWNS) == 0, "private namespace");
        require(mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == 0, "private propagation");
        require(write(ready[1], "x", 1) == 1, "namespace ready");
        char byte;
        require(read(finish[0], &byte, 1) == 1, "namespace release");
        auto inherited = inspect(disk, 1);
        require(rufus::UnmountNativeTarget(disk, -1, inherited, unmounted) == 0 && unmounted == 1, "inherited unmount");
        _exit(0);
    }
    close(ready[1]); close(finish[0]);
    char byte;
    require(read(ready[0], &byte, 1) == 1, "child namespace ready");
    require(rufus::UnmountNativeTarget(disk, -1, plan, unmounted) == 0 && unmounted == 1, "ordinary unmount");
    inspect(disk, 0);
    int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(claim < 0 && errno == EBUSY, "other namespace prevents exclusive claim");
    require(write(finish[1], "x", 1) == 1, "release child");
    int status;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "namespace child completed");
    close(ready[0]); close(finish[1]);
    claimFree();
    printf("RUFUS_MOUNTS_CASE namespace-copy ERROR=0\n");

    mountFirst(); mountSecond();
    plan = inspect(disk, 2);
    fault = Fault::CancelAfterFirst;
    require(rufus::UnmountNativeTarget(disk, -1, plan, unmounted) == ECANCELED && unmounted == 1, "cancel between volumes");
    fault = Fault::None;
    cancelled = false;
    plan = inspect(disk, 1);
    printf("RUFUS_MOUNTS_CASE partial-cancel ERROR=%d\n", ECANCELED);
    require(rufus::UnmountNativeTarget(disk, -1, plan, unmounted) == 0 && unmounted == 1, "remaining volume unmount");
    claimFree();
    printf("RUFUS_MOUNTS_CASE success ERROR=0\n");

    require(mount("/dev/sda1", "/home", "vfat", 0, "") == 0, "system mount");
    refusal(disk, -1, EBUSY, "system-mount");
    require(umount("/home") == 0, "system mount cleanup");

    mountFirst();
    runTool(QStringLiteral("/usr/bin/dmsetup"), {QStringLiteral("--noudevsync"), QStringLiteral("create"), QStringLiteral("rufus-mount-holder"),
        QStringLiteral("--table"), QStringLiteral("0 61440 linear /dev/sda2 0")});
    refusal(disk, -1, EBUSY, "partition-holder");
    // This minimal guest has no udev; devtmpfs publishes the first mapper node.
    const int layered = open("/dev/dm-0", O_RDONLY | O_CLOEXEC);
    require(layered >= 0, "layered device descriptor");
    refusal(layered, -1, EBUSY, "layered-device");
    close(layered);
    runTool(QStringLiteral("/usr/bin/dmsetup"), {QStringLiteral("--noudevsync"), QStringLiteral("remove"), QStringLiteral("rufus-mount-holder")});

    plan = inspect(disk, 1);
    runTool(QStringLiteral("/usr/bin/mkswap"), {QStringLiteral("/dev/sda2")});
    require(swapon("/dev/sda2", 0) == 0, "activate guest swap");
    refusal(disk, -1, EBUSY, "swap-partition");
    const int beforeSwap = unmountCalls;
    require(rufus::UnmountNativeTarget(disk, -1, plan, unmounted) == EBUSY && unmounted == 0, "swap blocks whole plan");
    require(unmountCalls == beforeSwap, "swap refusal precedes any unmount");
    require(swapoff("/dev/sda2") == 0, "guest swap cleanup");
    runTool(QStringLiteral("/usr/bin/mkfs.fat"), {QStringLiteral("-F"), QStringLiteral("16"), QStringLiteral("/dev/sda2")});
    plan = inspect(disk, 1);
    require(rufus::UnmountNativeTarget(disk, -1, plan, unmounted) == 0, "final mount cleanup");
    claimFree();
    require(mount("tmpfs", "/run/source", "tmpfs", 0, "size=1m") == 0, "independent memory source");
    const int independent = open("/run/source/image.iso", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    require(independent >= 0 && write(independent, "x", 1) == 1, "retained independent source");
    mountFirst(); mountSecond();
    const int home = retainedCallerHome(disk);
    require(rufus::CheckNativeTargetMounts(disk, independent, plan) == 0 && plan.volumes.size() == 2, "independent source eligible");
    printf("RUFUS_MOUNTS_CASE memory-source ERROR=0\n");
    require(rufus::UnmountNativeTarget(disk, independent, plan, unmounted, home) == 0 && unmounted == 2, "two-volume unmount");
    require(pread(independent, &byte, 1, 0) == 1 && byte == 'x', "independent source preserved");
    close(independent);
    close(home);
    require(umount("/run/source") == 0, "memory source cleanup");
    claimFree();
    printf("RUFUS_MOUNTS_CASE two-volumes ERROR=0\n");
    close(disk);
    const int threadDisk = open("/dev/sda", O_RDONLY | O_CLOEXEC);
    require(threadDisk >= 0, "thread fixture descriptor");
    threadNamespaces(threadDisk);
    if (QFile::exists(QStringLiteral("/etc/rufus-fuse-fixture")))
        fuseMounts(threadDisk);
    close(threadDisk);
    puts("RUFUS_MOUNTS_DONE");
    return 0;
}
