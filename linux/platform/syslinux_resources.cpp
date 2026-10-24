#include "iso_links.h"
// SPDX-License-Identifier: GPL-3.0-or-later
#include "syslinux_resources.h"
#include "syslinux_native.h"
#include "iso_file.h"
#include "image_file.h"
#include "syslinux_version.h"
#include <QFile>
#include <QtEndian>
#include <cerrno>
#include <cstring>
static bool registerResources() { Q_INIT_RESOURCE(syslinux_boot); return true; }
namespace rufus {
int ReadEmbeddedSyslinuxResources(const RUFUS_IMG_REPORT& original, SyslinuxResources& resources) {
    const RUFUS_IMG_REPORT report = SyslinuxSourceReport(original);
    static const bool registered = registerResources(); Q_UNUSED(registered);
    const QString prefix = SL_MAJOR(report.sl_version) < 5 ? QStringLiteral(":/rufus/syslinux/v4/") : QStringLiteral(":/rufus/syslinux/");
    QFile loader(prefix+QStringLiteral("ldlinux.sys")), boot(prefix+QStringLiteral("ldlinux.bss"));
    if (!loader.open(QIODevice::ReadOnly) || !boot.open(QIODevice::ReadOnly)) return EIO;
    resources.loader = loader.readAll(); resources.boot = boot.readAll();
    return resources.loader.isEmpty() || resources.boot.size() != 512 ? EIO : 0;
}
int ReadSyslinuxCompanion(const ISO_ANALYSIS_SOURCE& source, const RUFUS_IMG_REPORT& original, SyslinuxResources& resources) {
    const RUFUS_IMG_REPORT report = SyslinuxSourceReport(original);
    resources.module.clear(); if (SL_MAJOR(report.sl_version) < 5) return 0;
    const QByteArray selected(report.cfg_path);
    const QByteArray directory = selected.left(selected.lastIndexOf('/'));
    ISO_FILE_READER reader = {};
    int status = OpenResolvedIsoFile(&reader,source.iso,source.udf,"/ldlinux.c32");
    if (status != ISO_SCAN_OK) status = OpenResolvedIsoFile(&reader,source.iso,source.udf,(directory+"/ldlinux.c32").constData());
    if (status != ISO_SCAN_OK) return status == ISO_SCAN_CANCELLED ? ECANCELED : ENOENT;
    int error = reader.size >= 256 && reader.size <= 512*1024 ? 0 : EFBIG;
    if (!error) { resources.module.resize(qsizetype(reader.size)); status = ReadIsoFileRange(&reader,0,resources.module.data(),size_t(resources.module.size())); if (status != ISO_SCAN_OK) error = status == ISO_SCAN_CANCELLED ? ECANCELED : EIO; }
    CloseIsoFileReader(&reader);
    if (!error && report.sl_version_inferred) { QString problem; error = CheckNativeSyslinuxModule(resources.module,problem); }
    return error;
}
static bool fits(qsizetype offset, qsizetype count, qsizetype length) { return offset >= 0 && offset <= length && count >= 0 && count <= length-offset; }
int ValidateSyslinuxResources(const RUFUS_IMG_REPORT& original, const SyslinuxResources& resources) {
    const RUFUS_IMG_REPORT report = SyslinuxSourceReport(original);
    const int major = SL_MAJOR(report.sl_version);
    if (major < 3 || major > 6 || resources.loader.size() < 512 || resources.loader.size() > 512*1024 || resources.boot.size() != 512) return ENOTSUP;
    if (major < 5) { SyslinuxResources embedded; int error = ReadEmbeddedSyslinuxResources(report,embedded); if (error) return error; if (embedded.loader != resources.loader || embedded.boot != resources.boot) return ENOTSUP; }
    else {
        QByteArray banner = resources.loader; banner.append('\0'); char* suffix = nullptr;
        if (GetSyslinuxVersion(banner.data(),size_t(banner.size()),&suffix) != report.sl_version) return ENOTSUP;
        if (resources.module.size() < 256 || resources.module.size() > 512*1024) return EFBIG;
    }
    const auto* loader = reinterpret_cast<const uchar*>(resources.loader.constData());
    const auto* boot = reinterpret_cast<const uchar*>(resources.boot.constData());
    if (qFromLittleEndian<quint32>(boot+504) != 0x3eb202fe || boot[510] != 0x55 || boot[511] != 0xaa) return EBADMSG;
    qsizetype patch = -1;
    for (qsizetype i = 0; fits(i,24,resources.loader.size()); i += 4) if (qFromLittleEndian<quint32>(loader+i) == 0x3eb202fe) { patch = i; break; }
    if (patch < 0) return EBADMSG;
    const qsizetype epa = qFromLittleEndian<quint16>(loader+patch+22);
    if (!fits(epa,20,resources.loader.size())) return EBADMSG;
    const auto* fields = loader+epa;
    const qsizetype extents = qFromLittleEndian<quint16>(fields+10), count = qFromLittleEndian<quint16>(fields+12);
    if (count < (resources.loader.size()+511)/512-1) return ENOTSUP;
    const qsizetype offsets[] = {qFromLittleEndian<quint16>(fields), qFromLittleEndian<quint16>(fields+2),qFromLittleEndian<quint16>(fields+6),extents};
    const qsizetype sizes[] = {16,qFromLittleEndian<quint16>(fields+4),qFromLittleEndian<quint16>(fields+8),count*10};
    for (int i = 0; i < 4; ++i) {
        if (!fits(offsets[i],sizes[i],resources.loader.size())) return EBADMSG;
        if (sizes[i] && ((offsets[i] < epa+20 && epa < offsets[i]+sizes[i]) || (offsets[i] < patch+24 && patch < offsets[i]+sizes[i]))) return EBADMSG;
        for (int j = 0; j < i; ++j) if (sizes[i] && sizes[j] && offsets[i] < offsets[j]+sizes[j] && offsets[j] < offsets[i]+sizes[i]) return EBADMSG;
    }
    for (int field : {14,16}) if (!fits(qFromLittleEndian<quint16>(fields+field),4,512)) return EBADMSG;
    if (!fits(qFromLittleEndian<quint16>(fields+18),2,512)) return EBADMSG;
    return 0;
}
QByteArray EncodeSyslinuxResources(const SyslinuxResources& resources) {
    const QByteArray* parts[] = {&resources.loader,&resources.boot,&resources.module,&resources.oldModules[0],&resources.oldModules[1]};
    QByteArray bytes("RSLX0001",8); bytes.resize(28);
    for (int i = 0; i < 5; ++i) { if (parts[i]->size() > 512*1024) return {}; qToLittleEndian<quint32>(quint32(parts[i]->size()),bytes.data()+8+4*i); }
    for (const auto* part : parts) { if (bytes.size()+part->size() > 1024*1024-512) return {}; bytes.append(*part); }
    return bytes;
}
int DecodeSyslinuxResources(const QByteArray& bytes, SyslinuxResources& resources) {
    resources = {}; if (bytes.size() < 28 || bytes.size() > 1024*1024-512 || bytes.first(8) != "RSLX0001") return EBADMSG;
    SyslinuxResources decoded; QByteArray* parts[] = {&decoded.loader,&decoded.boot,&decoded.module,&decoded.oldModules[0],&decoded.oldModules[1]};
    qsizetype offset = 28;
    for (int i = 0; i < 5; ++i) { const qsizetype count = qFromLittleEndian<quint32>(bytes.constData()+8+4*i); if (count > 512*1024 || !fits(offset,count,bytes.size())) return EBADMSG; *parts[i] = bytes.mid(offset,count); offset += count; }
    if (offset != bytes.size()) return EBADMSG;
    resources = std::move(decoded); return 0;
}
}
