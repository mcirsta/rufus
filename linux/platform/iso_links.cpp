// SPDX-License-Identifier: GPL-3.0-or-later
#include "iso_links.h"
#include "image_file.h"
#include <QByteArray>
#include <QString>
#include <QDir>
#include <QSet>
#include <QtEndian>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
namespace {
int resolvePath(const QByteArray& parent, const QByteArray& target, QByteArray& path) {
    if (target.isEmpty() || target.contains('\\') || target.contains(':') || target.contains('\0')
        || QString::fromUtf8(target).toUtf8() != target) return EINVAL;
    QList<QByteArray> parts;
    const QByteArray joined = target.startsWith('/') ? target : parent + '/' + target;
    for (const auto& part : joined.split('/')) {
        if (part.isEmpty() || part == ".") continue;
        if (part == "..") { if (parts.isEmpty()) return EINVAL; parts.removeLast(); }
        else parts.append(part);
    }
    path = '/' + parts.join('/');
    if (path.size() >= ISO_SCAN_PATH_MAX) return ENAMETOOLONG;
    QByteArray sanitized = path;
    return SanitizeImagePath(sanitized.data()) ? EINVAL : 0;
}
}
extern "C" int PlanIsoLink(iso9660_t* iso, const ISO_TREE_ENTRY* entry, bool windows, bool syslinux, ISO_LINK_PLAN* plan) {
    *plan = {};
    if (!iso || !entry || !entry->iso || !entry->iso->rr.psz_symlink) return EINVAL;
    const QByteArray target(entry->iso->rr.psz_symlink);
    if (target.size() >= ISO_SCAN_PATH_MAX) return ENAMETOOLONG;
    QByteArray source;
    int error = resolvePath(entry->parent,target,source);
    if (error) return error;
    QSet<QByteArray> visited;
    bool found = false, exhausted = true;
    for (int depth = 0; depth < 32; ++depth) {
        if (visited.contains(source)) return ELOOP;
        visited.insert(source);
        iso9660_stat_t* file = iso9660_ifs_stat_translate(iso,source.constData());
        if (!file) { exhausted = false; break; }
        if (!file->rr.psz_symlink) {
            exhausted = false; found = true; plan->directory = file->type == iso9660_stat_t::_STAT_DIR;
            plan->size = file->total_size; iso9660_stat_free(file); break;
        }
        const QByteArray parent = source.left(source.lastIndexOf('/'));
        error = resolvePath(parent,file->rr.psz_symlink,source);
        iso9660_stat_free(file);
        if (error) return error;
    }
    if (exhausted) return ELOOP;
    if (windows && found) {
        plan->action = ISO_LINK_REPARSE;
        const QByteArray relative = QDir(QString::fromUtf8(entry->parent[0] ? entry->parent : "/")).relativeFilePath(QString::fromUtf8(source)).toUtf8();
        if (relative.size() >= ISO_SCAN_PATH_MAX) return ENAMETOOLONG;
        strcpy(plan->relative,relative.constData());
    } else if (!windows && entry->iso->total_size != 0) plan->action = ISO_LINK_SKIP;
    else if (!windows && strcasecmp(entry->name,"syslinux") == 0 && strcasecmp(target.constData(),"isolinux") == 0 && syslinux) {
        if (!found || !plan->directory) return ENOTDIR;
        plan->action = ISO_LINK_SYSLINUX; plan->size = 0;
    } else if (!windows && strcmp(entry->parent,"/firmware") == 0) {
        if (!found || plan->directory) return ENOENT;
        plan->action = ISO_LINK_DUPLICATE;
    } else { plan->action = ISO_LINK_LITERAL; plan->size = quint64(target.size()); }
    strcpy(plan->source,source.constData());
    return 0;
}
extern "C" int CreateNtfsIsoLink(int parent, const char* name, const ISO_LINK_PLAN* plan) {
    if (!plan || plan->action != ISO_LINK_REPARSE || !name || strchr(name,'/')) return EINVAL;
    QString target = QString::fromUtf8(plan->relative); target.replace('/', '\\');
    const qsizetype length = target.size() * 2;
    if (length <= 0 || length > 2048) return EFBIG;
    QByteArray reparse(20 + 2 * length,'\0');
    auto* data = reinterpret_cast<uchar*>(reparse.data());
    qToLittleEndian<quint32>(0xa000000c,data); qToLittleEndian<quint16>(quint16(12 + 2 * length),data+4);
    qToLittleEndian<quint16>(quint16(length),data+10); qToLittleEndian<quint16>(quint16(length),data+12);
    qToLittleEndian<quint16>(quint16(length),data+14); qToLittleEndian<quint32>(1,data+16);
    for (qsizetype i = 0; i < target.size(); ++i) {
        qToLittleEndian<quint16>(target[i].unicode(),data+20+2*i);
        qToLittleEndian<quint16>(target[i].unicode(),data+20+length+2*i);
    }
    if (plan->directory && mkdirat(parent,name,0755) != 0) return errno;
    const int flags = plan->directory ? O_RDONLY|O_DIRECTORY : O_WRONLY|O_CREAT|O_EXCL;
    const int file = openat(parent,name,flags|O_NOFOLLOW|O_CLOEXEC,0644);
    if (file < 0) return errno;
    int error = 0;
    if (fsetxattr(file,"system.ntfs_reparse_data",data,size_t(reparse.size()),0) != 0) error = errno;
    QByteArray actual(reparse.size(),'\0');
    if (!error && fgetxattr(file,"system.ntfs_reparse_data",actual.data(),size_t(actual.size())) != actual.size()) error = EIO;
    if (!error && actual != reparse) error = EIO;
    if (!error && fsync(file) != 0) error = errno;
    if (close(file) != 0 && !error) error = errno;
    return error;
}

extern "C" int RelocateIsoLink(char* path, ISO_LINK_PLAN* plan, const char* original, const char* destination)
{
    QByteArray name(path), source(plan->source), before(original), after(destination);
    if (name.left(before.size()).compare(before,Qt::CaseInsensitive) == 0 && name.mid(before.size()).startsWith('/')) name = after + name.mid(before.size());
    if (source.left(before.size()).compare(before,Qt::CaseInsensitive) == 0 && source.mid(before.size()).startsWith('/')) source = after + source.mid(before.size());
    const QByteArray parent = name.left(name.lastIndexOf('/'));
    const QByteArray relative = QDir(QString::fromUtf8(parent.isEmpty() ? "/" : parent)).relativeFilePath(QString::fromUtf8(source)).toUtf8();
    if (name.size() >= ISO_SCAN_PATH_MAX || source.size() >= ISO_SCAN_PATH_MAX || relative.size() >= ISO_SCAN_PATH_MAX) return ENAMETOOLONG;
    strcpy(path,name.constData()); strcpy(plan->source,source.constData()); strcpy(plan->relative,relative.constData());
    return 0;
}
extern "C" int OpenResolvedIsoFile(ISO_FILE_READER* reader, iso9660_t* iso, udf_t* udf, const char* path)
{
    if (!iso || udf) return OpenIsoFileReader(reader,iso,udf,path);
    iso9660_stat_t* file = iso9660_ifs_stat_translate(iso,path);
    if (!file) return ISO_SCAN_ERROR;
    QByteArray source(path);
    int error = 0;
    if (file->rr.psz_symlink) {
        ISO_TREE_ENTRY entry = {}; const QByteArray parent = source.left(source.lastIndexOf('/'));
        entry.iso = file; entry.parent = parent.constData(); entry.path = path; entry.name = path + source.lastIndexOf('/') + 1;
        ISO_LINK_PLAN plan;
        error = PlanIsoLink(iso,&entry,true,false,&plan);
        if (!error && plan.action == ISO_LINK_REPARSE && !plan.directory) source = plan.source;
        else if (!error) error = EINVAL;
    }
    iso9660_stat_free(file);
    return error ? ISO_SCAN_ERROR : OpenIsoFileReader(reader,iso,udf,source.constData());
}
