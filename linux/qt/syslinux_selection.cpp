// SPDX-License-Identifier: GPL-3.0-or-later
#include "syslinux_selection.h"
#include "platform/syslinux_native.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QRegularExpression>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
namespace rufus {
SyslinuxSelection::SyslinuxSelection(QObject* parent) : QObject(parent),download_(this) {
    connect(&download_,&SignedDownload::completed,this,&SyslinuxSelection::downloaded);
    connect(&download_,&SignedDownload::progress,this,&SyslinuxSelection::progress);
}
int SyslinuxSelection::readCache(const QString& path,QByteArray& bytes) {
    const QByteArray native = QFile::encodeName(QDir(directory_).filePath(path));
    IMAGE_FILE_SOURCE file = {-1,{},false,false,nullptr, 0};
    file.fd = open(native.constData(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (file.fd < 0) return errno;
    int error = fstat(file.fd,&file.info) == 0 ? 0 : errno;
    if (!error && (file.info.st_uid != getuid() || file.info.st_nlink != 1 || !(file.info.st_mode & S_IRUSR))) error = EACCES;
    if (!error) error = ReadPrepareCoreSource(file,native,512*1024,bytes);
    if (close(file.fd) != 0 && !error) error = errno;
    return error;
}
int SyslinuxSelection::start(const PrepareImageRequest& source,const RUFUS_IMG_REPORT& report,const SyslinuxResources& initial,const QString& directory,const QUrl& server) {
    if (active_) return EBUSY;
    const bool serverRoot = server.isValid() && (server.scheme() == "https" || server.scheme() == "http")
        && !server.host().isEmpty() && server.userInfo().isEmpty() && !server.hasQuery() && !server.hasFragment();
    if (!QDir::isAbsolutePath(directory) || directory.contains(QChar::Null) || !serverRoot) return EINVAL;
    request_ = source; report_ = SyslinuxSourceReport(report); resources_ = initial; directory_ = directory; server_ = server;
    missing_.clear(); active_ = true; waiting_ = false; base_ = false;
    source_ = std::make_unique<PrepareIsoSource>(request_);
    int error = source_->open(getuid()); if (error) { finish(error); return 0; }
    const int major = SL_MAJOR(report_.sl_version);
    QString version = QStringLiteral("%1.%2").arg(major).arg(SL_MINOR(report_.sl_version),2,10,QLatin1Char('0'));
    const QString suffix = QString::fromLatin1(report_.sl_version_ext);
    static const QRegularExpression extension(QStringLiteral("^(/[A-Za-z0-9._+ -]+)?$"));
    if (!extension.match(suffix).hasMatch() || suffix == "/." || suffix == "/..") { finish(EINVAL); return 0; }
    QList<Item> items;
    const bool pinned = report_.sl_version == nativeSyslinuxVersion && (suffix == "/pre1" || report_.sl_version_inferred);
    if (major >= 5 && !pinned) {
        for (const char* file : {"ldlinux.sys","ldlinux.bss"}) {
            const QString base = QStringLiteral("syslinux-%1/%2").arg(version,QString::fromLatin1(file));
            items.append({QStringLiteral("syslinux-%1%2/%3").arg(version,suffix,QString::fromLatin1(file)),base,strcmp(file,"ldlinux.sys") == 0 ? 0 : 1,false});
        }
    }
    if (major < 5) for (int i = 0; i < 2; ++i) if (report.has_old_c32[i]) {
        const QString path = QStringLiteral("syslinux-4.07/%1").arg(i == 0 ? QStringLiteral("menu.c32") : QStringLiteral("vesamenu.c32"));
        items.append({path,path,3+i,true});
    }
    QByteArray* parts[] = {&resources_.loader,&resources_.boot,&resources_.module,&resources_.oldModules[0],&resources_.oldModules[1]};
    for (const Item& item : items) {
        QByteArray bytes; error = readCache(item.full,bytes);
        if (!error) *parts[item.part] = bytes;
        else if (error == ENOENT || error == ENOTDIR) missing_.append(item);
        else { finish(error); return 0; }
    }
    if (missing_.isEmpty()) finish(0);
    else { waiting_ = true; emit downloadRequired(QStringLiteral("Syslinux %1%2").arg(version,suffix)); }
    return 0;
}
void SyslinuxSelection::choose(Grub2DownloadChoice choice) {
    if (!active_ || !waiting_) return;
    waiting_ = false;
    if (choice == Grub2DownloadChoice::Cancel) { finish(ECANCELED); return; }
    if (choice == Grub2DownloadChoice::No) {
        for (const Item& item : missing_) if (!item.optional) { finish(ENOENT); return; }
        missing_.clear(); finish(0); return;
    }
    next();
}
void SyslinuxSelection::next() {
    int error = source_->check(); if (error) { finish(ESTALE); return; }
    if (missing_.isEmpty()) { finish(0); return; }
    QUrl url = server_; QString path = url.path(); if (!path.endsWith('/')) path += '/';
    path += base_ ? missing_.first().base : missing_.first().full; url.setPath(path);
    error = download_.start(url,512*1024); if (error) finish(error);
}
void SyslinuxSelection::downloaded(SignedDownloadResult result) {
    if (!active_) return;
    if (result.error && result.httpStatus == 404 && !result.signatureRequest && !base_ && missing_.first().full != missing_.first().base) { base_ = true; next(); return; }
    if (result.error && result.httpStatus == 404 && !result.signatureRequest && report_.sl_version == nativeSyslinuxVersion) {
        SyslinuxResources embedded;
        const int error = ReadEmbeddedSyslinuxResources(report_,embedded);
        if (error) { finish(error); return; }
        resources_.loader = embedded.loader; resources_.boot = embedded.boot;
        missing_.clear(); finish(0); return;
    }
    if (result.error) { finish(result.error); return; }
    const int error = source_->check(); if (error) { finish(ESTALE); return; }
    const Item item = missing_.takeFirst();
    const QString path = QDir(directory_).filePath(item.full);
    if (!QDir().mkpath(QFileInfo(path).path())) { finish(EIO); return; }
    QSaveFile output(path); output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(result.bytes) != result.bytes.size() || !output.commit()) { finish(EIO); return; }
    QByteArray reread; const int read = readCache(item.full,reread);
    if (read || reread != result.bytes) { finish(read ? read : EIO); return; }
    QByteArray* parts[] = {&resources_.loader,&resources_.boot,&resources_.module,&resources_.oldModules[0],&resources_.oldModules[1]};
    *parts[item.part] = result.bytes; base_ = false; next();
}
void SyslinuxSelection::finish(int error) {
    if (!active_) return;
    if (!error) error = source_->check();
    if (!error) error = ValidateSyslinuxResources(report_,resources_);
    QByteArray bytes = !error ? EncodeSyslinuxResources(resources_) : QByteArray();
    if (!error && bytes.isEmpty()) error = EFBIG;
    active_ = waiting_ = false; source_.reset(); emit completed(error,error ? QByteArray() : bytes);
}
void SyslinuxSelection::cancel() { if (!active_) return; download_.cancel(); if (active_) finish(ECANCELED); }
}
