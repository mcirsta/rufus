#include "format_policy.h"
#include "iso_links.h"
// SPDX-License-Identifier: GPL-3.0-or-later
#include "syslinux_source.h"
#include "syslinux_native.h"
#include "legacy_iso.h"
#include "image_file.h"
#include "iso_file.h"
#include <QMap>
#include <cerrno>
#include <sys/stat.h>

namespace rufus
{
namespace
{
struct SourceEntry
{
    QByteArray path;
    bool directory = false;
    bool ancestor = false;
};

struct SourceLayout
{
    QMap<QByteArray, SourceEntry> entries;
    bool rockRidge = false;
    bool windows = false;
    iso9660_t* iso = nullptr;
    int error = 0;
    QString problem;
};

QByteArray fatPathKey(const QByteArray& path)
{
    // Kernel VFAT ignores trailing dots and rejects trailing spaces. Fold both
    // here only to reject unsupported spellings of watched boot paths; mtools
    // name handling differs. Never use this key to rename extracted output.
    QList<QByteArray> components = path.toLower().split('/');
    for (QByteArray& component : components)
        while (component.endsWith('.') || component.endsWith(' '))
            component.chop(1);
    return components.join('/');
}

// Only store boot paths and their parents, not the entire ISO inventory.
void watchPath(SourceLayout& layout, QByteArray path)
{
    path = path.toLower();
    if (!layout.entries.contains(path))
        layout.entries.insert(path, {});
    while (path.lastIndexOf('/') > 0)
    {
        path.truncate(path.lastIndexOf('/'));
        layout.entries[path].ancestor = true;
    }
}

int visitBootEntry(const ISO_TREE_ENTRY* entry, void* data)
{
    SourceLayout& layout = *static_cast<SourceLayout*>(data);
    if (entry->kind == ISO_TREE_DIRECTORY_END)
        return ISO_SCAN_OK;
    if (entry->kind == ISO_TREE_DEEP)
    {
        layout.error = ENOTSUP;
        layout.problem = QStringLiteral("Syslinux source traversal is incomplete.");
        return ISO_SCAN_ERROR;
    }
    QByteArray outputPath(entry->path);
    const bool sanitized = SanitizeImagePath(outputPath.data());
    const QByteArray key = fatPathKey(outputPath);
    const bool renamed = sanitized || key != outputPath.toLower();
    auto found = layout.entries.find(key);
    if (found == layout.entries.end())
        return ISO_SCAN_OK;
    SourceEntry& file = found.value();
    bool directory = entry->kind == ISO_TREE_DIRECTORY;
    bool unsupportedType = file.ancestor && !directory;
    if (entry->iso != nullptr && layout.rockRidge && entry->iso->rr.b3_rock == yep)
    {
        const mode_t mode = iso9660_get_posix_filemode(entry->iso);
        const bool ordinary = S_ISREG(mode) || S_ISDIR(mode);
        unsupportedType |= !ordinary || entry->iso->rr.psz_symlink != nullptr;
        if (entry->iso->rr.psz_symlink) {
            ISO_LINK_PLAN plan;
            const int error = PlanIsoLink(layout.iso,entry,layout.windows,true,&plan);
            if (!error && (plan.action == ISO_LINK_REPARSE || plan.action == ISO_LINK_SYSLINUX)) {
                directory = plan.directory;
                unsupportedType = file.ancestor && !directory;
            }
        }
    }
    if (entry->udf != nullptr)
    {
        const uint8_t type = entry->udf->fe.icb_tag.file_type;
        const bool ordinary = type == ICBTAG_FILE_TYPE_REGULAR || type == ICBTAG_FILE_TYPE_DIRECTORY;
        unsupportedType |= !ordinary;
    }
    if (!file.path.isEmpty())
    {
        layout.error = EEXIST;
        layout.problem = QStringLiteral("Syslinux boot paths collide on the target: %1 and %2.")
            .arg(QString::fromUtf8(file.path), QString::fromUtf8(entry->path));
    }
    else if (renamed || unsupportedType)
    {
        layout.error = ENOTSUP;
        layout.problem = QStringLiteral("Syslinux needs an unchanged ordinary boot path: %1.")
            .arg(QString::fromUtf8(entry->path));
    }
    if (layout.error != 0)
        return ISO_SCAN_ERROR;
    file.path = entry->path;
    file.directory = directory;
    return ISO_SCAN_OK;
}

int checkModule(const ISO_ANALYSIS_SOURCE& source, const RUFUS_IMG_REPORT& report,
    const SourceEntry& module, int filesystem, quint32 sectorBytes, quint64 offsetBytes,
    bool replaced, QString& problem)
{
    if (module.path.isEmpty() || module.directory)
    {
        problem = QStringLiteral("The selected Syslinux directory needs an ordinary ldlinux.c32 companion.");
        return ENOTSUP;
    }
    // Win32 selects by the detected version/suffix, not a hash of modules it
    // preserves. The native root installer keeps this non-root file too.
    // Unknown inference and root replacement still need the exact pinned pair.
    Q_UNUSED(replaced);
    const bool needsExactPair = report.sl_version_inferred;
    if (!needsExactPair)
        return CheckNativeSyslinuxSelection(report, filesystem, sectorBytes, offsetBytes, problem);
    ISO_FILE_READER reader = {};
    int status = OpenResolvedIsoFile(&reader, source.iso, source.udf, module.path.constData());
    int error = status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
    problem = QStringLiteral("Cannot read the Syslinux companion: %1.").arg(QString::fromUtf8(module.path));
    if (status == ISO_SCAN_OK)
    {
        if (reader.size != quint64(nativeSyslinuxModuleBytes))
        {
            error = ENOTSUP;
            problem = QStringLiteral("The Syslinux companion has an unsupported size: %1.")
                .arg(QString::fromUtf8(module.path));
        }
        else
        {
            QByteArray bytes(nativeSyslinuxModuleBytes, '\0');
            status = ReadIsoFileRange(&reader, 0, bytes.data(), size_t(bytes.size()));
            if (status == ISO_SCAN_OK)
                error = CheckNativeSyslinuxPair(report, filesystem, sectorBytes, offsetBytes, bytes, problem);
            else
                error = status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
        }
    }
    CloseIsoFileReader(&reader);
    return error;
}

int checkConfigRoute(const SourceLayout& layout, const QList<QByteArray>& directories,
    const RUFUS_IMG_REPORT& report, const QByteArray& selected, QString& problem)
{
    const SourceEntry root = layout.entries.value("/syslinux.cfg");
    if (root.directory || layout.entries.value("/ldlinux.sys").directory)
    {
        problem = QStringLiteral("A directory occupies a reserved Syslinux root file.");
        return ENOTSUP;
    }
    ISO_SYSLINUX_CONFIG redirect = {};
    GetIsoSyslinuxConfig(&report, !root.path.isEmpty(), &redirect);
    if (redirect.backup && !layout.entries.value("/syslinux.org").path.isEmpty())
    {
        problem = QStringLiteral("The required Syslinux backup would overwrite /syslinux.org.");
        return EEXIST;
    }
    // Extraction may duplicate EFI/BOOT/isolinux.cfg as syslinux.cfg. Use the
    // shared rule and compare the copied content's origin, not just its name.
    const QByteArray parent = selected.left(selected.lastIndexOf('/'));
    const QByteArray name = selected.mid(selected.lastIndexOf('/') + 1);
    ISO_FILE_PROPS props = {};
    GetIsoFileProps(parent.constData(), 0, name.constData(), &props);
    char duplicateName[ISO_SYSLINUX_CONFIG_NAME_CAPACITY];
    QByteArray duplicate;
    if (GetEfiSyslinuxConfigName(&report, &props, parent.constData(), name.constData(), duplicateName))
    {
        const QByteArray path = parent + '/' + duplicateName;
        const SourceEntry destination = layout.entries.value(path);
        if (destination.directory)
        {
            problem = QStringLiteral("A directory occupies the required EFI Syslinux config copy.");
            return ENOTSUP;
        }
        if (destination.path.isEmpty())
            duplicate = path;
    }
    // The selected-directory search is the one used by Rufus's patched loader.
    // The native root installer finds root extlinux.conf, then root syslinux.cfg.
    QByteArray windowsConfig;
    const QList<QByteArray> filenames = {"/extlinux.conf", "/syslinux.cfg"};
    for (const QByteArray& directory : directories)
    {
        for (const QByteArray& name : filenames)
        {
            const QByteArray path = directory + name;
            const SourceEntry file = layout.entries.value(path);
            const bool generated = (path == "/syslinux.cfg" && redirect.create) || path == duplicate;
            const bool ordinary = !file.path.isEmpty() && !file.directory;
            if (generated || ordinary)
            {
                windowsConfig = path;
                break;
            }
        }
        if (!windowsConfig.isEmpty())
            break;
    }
    const SourceEntry rootExtlinux = layout.entries.value("/extlinux.conf");
    QByteArray nativeConfig = !rootExtlinux.path.isEmpty() && !rootExtlinux.directory
        ? "/extlinux.conf" : "/syslinux.cfg";
    if (redirect.create)
    {
        if (windowsConfig == "/syslinux.cfg")
            windowsConfig = selected;
        if (nativeConfig == "/syslinux.cfg")
            nativeConfig = selected;
    }
    if (!duplicate.isEmpty() && windowsConfig == duplicate)
        windowsConfig = selected;
    if (windowsConfig != nativeConfig)
    {
        problem = QStringLiteral("Syslinux config lookup differs: Rufus would load %1, but the native root installer would load %2.")
            .arg(QString::fromUtf8(windowsConfig), QString::fromUtf8(nativeConfig));
        return ENOTSUP;
    }
    return 0;
}
}

int CheckNativeSyslinuxSource(const ISO_ANALYSIS_SOURCE& source, const ISO_ANALYSIS& analysis,
    bool rockRidge, int filesystem, quint32 sectorBytes, quint64 offsetBytes, QString& problem)
{
    problem.clear();
    const bool isoView = source.iso != nullptr && source.udf == nullptr && analysis.filesystem == ISO_FILESYSTEM_ISO9660;
    const bool udfView = source.udf != nullptr && source.iso == nullptr && analysis.filesystem == ISO_FILESYSTEM_UDF;
    if (!isoView && !udfView)
    {
        problem = QStringLiteral("Syslinux source checks require the analysis's selected filesystem view.");
        return EINVAL;
    }
    const quint32 incomplete = ISO_PENDING_DEEP_DIRECTORIES | ISO_PENDING_TRUNCATED | ISO_PENDING_CONTENT;
    if (!analysis.tree_complete || (analysis.pending & incomplete) != 0)
    {
        problem = QStringLiteral("Syslinux source analysis is incomplete.");
        return ENOTSUP;
    }
    const RUFUS_IMG_REPORT& report = analysis.report;
    if (HAS_REACTOS(report) && !HAS_SYSLINUX(report))
    {
        const int error = CheckLegacyIsoFiles(source,report,true,problem,rockRidge);
        return error ? error : CheckNativeSyslinuxSelection(report,filesystem,sectorBytes,offsetBytes,problem);
    }
    const QByteArray selected = QByteArray(report.cfg_path).toLower();
    if (!HAS_SYSLINUX(report) || !selected.startsWith('/') || selected.endsWith('/'))
    {
        problem = QStringLiteral("There is no selected Syslinux configuration.");
        return ENOTSUP;
    }
    // The shared CONFIG/APPEND redirect has no filename quoting. Keep this
    // qualification to paths that survive extraction and its token parsing.
    QByteArray sanitized = selected;
    bool supportedPath = !SanitizeImagePath(sanitized.data()) && fatPathKey(selected) == selected;
    for (char c : selected)
        supportedPath &= c > ' ' && c < 127 && c != '\\' && c != '"';
    if (!supportedPath)
    {
        problem = QStringLiteral("The selected Syslinux path is not supported by the qualified root redirect.");
        return ENOTSUP;
    }
    const QByteArray selectedDirectory = selected.left(selected.lastIndexOf('/'));
    const QByteArray selectedModule = selectedDirectory + "/ldlinux.c32";
    QList<QByteArray> directories = {"/boot/syslinux", "/syslinux", ""};
    // InstallSyslinux truncates cfg_path only when its final slash is not the
    // first byte. For a root config it passes the full filename as the start
    // directory, whose lookup cannot succeed. Preserve the fallback order.
    if (!selectedDirectory.isEmpty())
        directories.prepend(selectedDirectory);
    SourceLayout layout;
    layout.rockRidge = rockRidge; layout.iso = source.iso; layout.windows = filesystem == FS_NTFS;
    for (const QByteArray& directory : directories)
    {
        watchPath(layout, directory + "/extlinux.conf");
        watchPath(layout, directory + "/syslinux.cfg");
    }
    for (const QByteArray& path : {selected, selectedModule, QByteArray("/ldlinux.c32"),
        QByteArray("/ldlinux.sys"), QByteArray("/syslinux.org")})
        watchPath(layout, path);
    ISO_TREE_WALK walk = {visitBootEntry, &layout, rockRidge, 0};
    if (isoView)
        walk.joliet_level = iso9660_ifs_get_joliet_level(source.iso);
    const int status = udfView ? WalkUdfFiles(udf_get_root(source.udf, true, 0), "", &walk)
        : WalkIsoFiles(source.iso, "", &walk);
    if (status != ISO_SCAN_OK)
    {
        problem = layout.problem.isEmpty() ? QStringLiteral("Cannot complete Syslinux source traversal.") : layout.problem;
        return layout.error != 0 ? layout.error : (status == ISO_SCAN_CANCELLED ? ECANCELED : EIO);
    }
    const SourceEntry config = layout.entries.value(selected);
    if (config.path.isEmpty() || config.directory)
    {
        problem = QStringLiteral("The selected Syslinux configuration is not an ordinary source file.");
        return ENOTSUP;
    }
    const bool selectedAtRoot = selectedModule == "/ldlinux.c32";
    // Original InstallSyslinux creates ReactOS's root config before extraction.
    // It participates in GetIsoSyslinuxConfig's existence/backup decisions even
    // when the selected source config lives in another directory.
    if (HAS_REACTOS(report) && layout.entries.value("/syslinux.cfg").path.isEmpty())
        layout.entries["/syslinux.cfg"].path = "/syslinux.cfg";
    int error = checkConfigRoute(layout, directories, report, selected, problem);
    if (error == 0 && SL_MAJOR(report.sl_version) >= 5)
        error = checkModule(source, report, layout.entries.value(selectedModule), filesystem,
            sectorBytes, offsetBytes, selectedAtRoot, problem);
    const SourceEntry rootModule = layout.entries.value("/ldlinux.c32");
    if (error == 0 && SL_MAJOR(report.sl_version) >= 5 && !selectedAtRoot && !rootModule.path.isEmpty())
        error = checkModule(source, report, rootModule, filesystem, sectorBytes, offsetBytes, true, problem);
    if (!error && HAS_REACTOS(report)) error = CheckLegacyIsoFiles(source,report,true,problem,rockRidge);
    return error;
}
}
