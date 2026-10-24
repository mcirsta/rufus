// SPDX-License-Identifier: GPL-3.0-or-later
#include "legacy_iso.h"
#include "image_file.h"
#include "iso_file.h"
#include "rufus_reporting.h"
#include <QFile>
#include <QRandomGenerator>
#include <QtEndian>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include "filesystem_magic.h"
#include <linux/msdos_fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/xattr.h>
#include <unistd.h>

static bool registerLegacyResources()
{
    Q_INIT_RESOURCE(syslinux_boot);
    return true;
}

namespace rufus
{
namespace
{
constexpr size_t transferBytes = 64 * 1024;
constexpr char kolibriSource[] = "/HD_Load/USB_Boot/MTLD_F32";
constexpr long ntfsMagic = 0x5346544e;

int scanError(int status)
{
    return status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
}

bool unchangedLoaderPath(const char* path)
{
    QByteArray name(path);
    if (!name.startsWith('/') || name.endsWith('/') || name.contains('\r') || name.contains('\n'))
        return false;
    return !SanitizeImagePath(name.data());
}

struct LegacySourceCheck
{
    QByteArray loader;
    bool reactos = false, kolibri = false;
    bool loaderFound = false, kolibriFound = false;
    quint64 kolibriBytes = 0;
    bool rockRidge = true;
    int error = 0;
    QString problem;
};

int checkSourceEntry(const ISO_TREE_ENTRY* entry, void* context)
{
    LegacySourceCheck& check = *static_cast<LegacySourceCheck*>(context);
    if (entry->kind == ISO_TREE_DIRECTORY_END)
        return ISO_SCAN_OK;
    const QByteArray path = QByteArray(entry->path).toLower();
    const bool loader = check.reactos && path == check.loader;
    const bool kolibri = check.kolibri && path == QByteArray(kolibriSource).toLower();
    const bool root = (check.reactos && (path == "/mboot.c32" || path == "/syslinux.cfg" || path == "/ldlinux.sys")) ||
        (check.kolibri && path == "/mtld_f32");
    if (!loader && !kolibri && !root)
        return ISO_SCAN_OK;
    bool ordinary = entry->kind == ISO_TREE_FILE;
    if (ordinary && check.rockRidge && entry->iso && entry->iso->rr.b3_rock == yep)
        ordinary = S_ISREG(iso9660_get_posix_filemode(entry->iso)) && !entry->iso->rr.psz_symlink;
    if (ordinary && entry->udf)
        ordinary = entry->udf->fe.icb_tag.file_type == ICBTAG_FILE_TYPE_REGULAR;
    if (!ordinary)
    {
        check.error = ENOTSUP;
        check.problem = QStringLiteral("Legacy ISO boot files must be ordinary source files: %1.")
            .arg(QString::fromUtf8(entry->path));
        return ISO_SCAN_ERROR;
    }
    if (loader) check.loaderFound = true;
    if (kolibri)
    {
        check.kolibriFound = true;
        check.kolibriBytes = entry->iso ? entry->iso->total_size : udf_get_file_length(entry->udf);
    }
    return ISO_SCAN_OK;
}

int checkDirectory(int directory, bool& ntfs, dev_t& device)
{
    const int flags = fcntl(directory, F_GETFL);
    if (flags < 0) return errno;
    if (flags & O_PATH) return EINVAL;
    struct stat info;
    if (fstat(directory, &info) != 0) return errno;
    if (!S_ISDIR(info.st_mode)) return ENOTDIR;
    struct statfs filesystem;
    if (fstatfs(directory, &filesystem) != 0) return errno;
    ntfs = filesystem.f_type == ntfsMagic || filesystem.f_type == FUSE_SUPER_MAGIC;
    if (!ntfs && filesystem.f_type != MSDOS_SUPER_MAGIC) return ENOTSUP;
    device = info.st_dev;
    return 0;
}

int syncFile(int file, LegacyIsoResult& result)
{
    while (fsync(file) != 0)
    {
        if (errno == EINTR) continue;
        if (!result.syncError) result.syncError = errno;
        return errno;
    }
    return 0;
}

int setLoaderAttributes(int file, bool ntfs)
{
    constexpr quint32 systemHidden = ATTR_HIDDEN | ATTR_SYS;
    constexpr char attributeName[] = "system.ntfs_attrib";
    quint32 attributes = 0, encoded = 0;
    if (ntfs)
    {
        const ssize_t count = fgetxattr(file, attributeName, &encoded, sizeof(encoded));
        if (count < 0) return errno;
        if (count != sizeof(encoded)) return EBADMSG;
        attributes = qFromLittleEndian(encoded);
    }
    else if (ioctl(file, FAT_IOCTL_GET_ATTRIBUTES, &attributes) != 0)
        return errno;
    const quint32 expected = (attributes | systemHidden) & ~quint32(0x80);
    if (ntfs)
    {
        encoded = qToLittleEndian(expected);
        if (fsetxattr(file, attributeName, &encoded, sizeof(encoded), 0) != 0) return errno;
        const ssize_t count = fgetxattr(file, attributeName, &encoded, sizeof(encoded));
        if (count < 0) return errno;
        if (count != sizeof(encoded)) return EBADMSG;
        attributes = qFromLittleEndian(encoded);
    }
    else
    {
        if (ioctl(file, FAT_IOCTL_SET_ATTRIBUTES, &expected) != 0) return errno;
        if (ioctl(file, FAT_IOCTL_GET_ATTRIBUTES, &attributes) != 0) return errno;
    }
    return attributes == expected ? 0 : EBADMSG;
}

int checkExistingFile(int directory, const char* name, dev_t device)
{
    struct stat info;
    if (fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? 0 : errno;
    return S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_dev == device ? 0 : ENOTSUP;
}

int writeLegacyFile(int directory, const char* name, const QByteArray& bytes,
    ISO_FILE_READER* source, bool system, bool ntfs, dev_t device, LegacyIsoResult& result)
{
    result.path = QString::fromLatin1(name);
    if (IsRufusJobCancelled()) return ECANCELED;
    int error = checkExistingFile(directory, name, device);
    if (error) return error;
    QByteArray temporary;
    int file = -1;
    for (int attempt = 0; attempt < 32; ++attempt)
    {
        temporary = QByteArray(".rufus-legacy-") + QByteArray::number(QRandomGenerator::global()->generate64(),16);
        file = openat(directory, temporary.constData(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (file >= 0 || errno != EEXIST) break;
    }
    if (file < 0) return errno;
    const quint64 size = source ? source->size : quint64(bytes.size());
    char expected[transferBytes], actual[transferBytes];
    quint64 done = 0;
    while (!error && done < size)
    {
        if (IsRufusJobCancelled()) { error = ECANCELED; break; }
        const size_t count = size_t(qMin<quint64>(transferBytes,size-done));
        if (source)
        {
            const int status = ReadIsoFileRange(source,done,expected,count);
            if (status != ISO_SCAN_OK) { error = scanError(status); break; }
        }
        else memcpy(expected,bytes.constData()+done,count);
        size_t written = 0;
        while (written < count)
        {
            const ssize_t transferred = write(file,expected+written,count-written);
            if (transferred < 0 && errno == EINTR) continue;
            if (transferred <= 0) { error = transferred == 0 ? EIO : errno; break; }
            written += size_t(transferred); result.bytes += quint64(transferred);
        }
        size_t read = 0;
        while (!error && read < count)
        {
            const ssize_t transferred = pread(file,actual+read,count-read,off_t(done+read));
            if (transferred < 0 && errno == EINTR) continue;
            if (transferred <= 0) { error = transferred == 0 ? EIO : errno; break; }
            read += size_t(transferred);
        }
        if (!error && memcmp(actual,expected,count) != 0) error = EBADMSG;
        done += written;
    }
    if (!error && system) error = setLoaderAttributes(file,ntfs);
    if (!error) error = syncFile(file,result);
    if (close(file) != 0 && !error) error = errno;
    if (!error) error = checkExistingFile(directory,name,device);
    if (!error && IsRufusJobCancelled()) error = ECANCELED;
    if (!error && renameat(directory,temporary.constData(),directory,name) != 0) error = errno;
    if (error) unlinkat(directory,temporary.constData(),0);
    else
    {
        ++result.files;
        error = syncFile(directory,result);
    }
    if (!error) result.path.clear();
    return error;
}
}

int ReadReactOsIsoFiles(const RUFUS_IMG_REPORT& report, QByteArray& mboot, QByteArray& config)
{
    mboot.clear(); config.clear();
    if (!HAS_REACTOS(report) || !unchangedLoaderPath(report.reactos_path)) return ENOTSUP;
    static const bool registered = registerLegacyResources(); Q_UNUSED(registered);
    QFile resource(QStringLiteral(":/rufus/syslinux/mboot.c32"));
    if (!resource.open(QIODevice::ReadOnly)) return EIO;
    mboot = resource.readAll();
    if (mboot.isEmpty()) return EIO;
    config = QByteArray("DEFAULT ReactOS\nLABEL ReactOS\n  KERNEL mboot.c32\n  APPEND ") + report.reactos_path;
    return 0;
}

int CheckLegacyIsoFiles(const ISO_ANALYSIS_SOURCE& source, const RUFUS_IMG_REPORT& report,
    bool reactosSyslinux, QString& problem, bool rockRidge, quint64* extraBytes)
{
    problem.clear();
    if (extraBytes) *extraBytes = 0;
    if (!reactosSyslinux && !HAS_KOLIBRIOS(report)) return 0;
    if ((source.iso == nullptr) == (source.udf == nullptr)) return EINVAL;
    if (reactosSyslinux && (!HAS_REACTOS(report) || !unchangedLoaderPath(report.reactos_path)))
    {
        problem = QStringLiteral("ReactOS needs an unchanged ordinary freeldr.sys or setupldr.sys source path.");
        return ENOTSUP;
    }
    LegacySourceCheck check;
    check.loader = QByteArray(report.reactos_path).toLower();
    check.reactos = reactosSyslinux; check.kolibri = HAS_KOLIBRIOS(report);
    check.rockRidge = rockRidge;
    ISO_TREE_WALK walk = {checkSourceEntry,&check,rockRidge,0};
    if (source.iso) walk.joliet_level = iso9660_ifs_get_joliet_level(source.iso);
    const int status = source.iso ? WalkIsoFiles(source.iso,"",&walk) :
        WalkUdfFiles(udf_get_root(source.udf,true,0),"",&walk);
    int error = status == ISO_SCAN_OK ? 0 : check.error ? check.error : scanError(status);
    if (!error && ((check.reactos && !check.loaderFound) || (check.kolibri && !check.kolibriFound)))
        error = ENOENT;
    quint64 additional = check.kolibriBytes;
    if (!error && reactosSyslinux)
    {
        QByteArray mboot, config;
        error = ReadReactOsIsoFiles(report,mboot,config);
        const quint64 reactosBytes = quint64(mboot.size()) + quint64(config.size());
        if (!error && additional > quint64(INT64_MAX) - reactosBytes) error = EOVERFLOW;
        if (!error) additional += reactosBytes;
    }
    if (error) problem = check.problem.isEmpty() ?
        QStringLiteral("The selected legacy ISO is missing a usable ReactOS or Kolibri boot resource.") : check.problem;
    if (!error && extraBytes) *extraBytes = additional;
    return error;
}

int InstallReactOsIsoFiles(const RUFUS_IMG_REPORT& report, int directory, LegacyIsoResult& result)
{
    result = {};
    QByteArray mboot, config;
    int error = ReadReactOsIsoFiles(report,mboot,config);
    bool ntfs = false; dev_t device = 0;
    if (!error) error = checkDirectory(directory,ntfs,device);
    if (!error) error = writeLegacyFile(directory,"mboot.c32",mboot,nullptr,false,ntfs,device,result);
    if (!error) error = writeLegacyFile(directory,"syslinux.cfg",config,nullptr,false,ntfs,device,result);
    return error;
}

int InstallKolibriIsoLoader(const ISO_ANALYSIS_SOURCE& source, int directory, LegacyIsoResult& result)
{
    result = {};
    ISO_FILE_READER reader = {};
    const int status = OpenIsoFileReader(&reader,source.iso,source.udf,kolibriSource);
    int error = status == ISO_SCAN_OK ? 0 : scanError(status);
    bool ntfs = false; dev_t device = 0;
    if (!error) error = checkDirectory(directory,ntfs,device);
    if (!error) error = writeLegacyFile(directory,"MTLD_F32",{},&reader,true,ntfs,device,result);
    CloseIsoFileReader(&reader);
    return error;
}
}
