// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_to_go.h"
#include "prepare_source.h"
#include "format_options.h"
#include "windows_customization.h"
#include "wim_file.h"
#include "wim_read_file.h"
#include "pe_arch.h"
#include "rufus_reporting.h"
extern "C" {
#include "native_input.h"
}
#include <QFile>
#include <QTemporaryFile>
#include <libfdisk.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace rufus
{
namespace
{
int nativeWimError(int error)
{
    if (error == 0) return 0;
    if (error == WIMLIB_ERR_ABORTED_BY_PROGRESS) return ECANCELED;
    if (error == WIMLIB_ERR_NOMEM) return ENOMEM;
    if (error == WIMLIB_ERR_INVALID_IMAGE || error == WIMLIB_ERR_INVALID_PARAM) return EINVAL;
    return EIO;
}

int regularBootFile(const wimlib_dir_entry* entry, void*)
{
    const unsigned unsupported = WIMLIB_FILE_ATTRIBUTE_DIRECTORY | WIMLIB_FILE_ATTRIBUTE_DEVICE
        | WIMLIB_FILE_ATTRIBUTE_REPARSE_POINT | WIMLIB_FILE_ATTRIBUTE_ENCRYPTED;
    return entry->attributes & unsupported ? WIMLIB_ERR_NOT_A_REGULAR_FILE : 0;
}

int bootTreeEntry(const wimlib_dir_entry* entry, void*)
{
    if (IsRufusJobCancelled() || IsIsoScanCancelled()) return WIMLIB_ERR_ABORTED_BY_PROGRESS;
    const unsigned unsupported = WIMLIB_FILE_ATTRIBUTE_DEVICE | WIMLIB_FILE_ATTRIBUTE_REPARSE_POINT | WIMLIB_FILE_ATTRIBUTE_ENCRYPTED;
    return entry->attributes & unsupported ? WIMLIB_ERR_NOT_A_REGULAR_FILE : 0;
}

int checkTemplate(WIMStruct* wim, int image, bool bios, bool uefi)
{
    uint8_t* bytes = nullptr;
    size_t size = 0;
    int error = nativeWimError(ReadWimFile(wim, image, "Windows/System32/config/BCD-Template", &bytes, &size));
    QTemporaryFile input;
    if (error == 0 && !input.open()) error = EIO;
    if (error == 0 && (input.write(reinterpret_cast<char*>(bytes), qint64(size)) != qint64(size) || !input.flush())) error = EIO;
    free(bytes);
    WindowsBcdOptions options;
    options.windows.mbrSignature = options.system.mbrSignature = 1;
    options.windows.offset = options.system.offset = 1024 * 1024;
    for (int firmware = 0; error == 0 && firmware < 2; ++firmware)
    {
        if ((firmware == 0 && !bios) || (firmware == 1 && !uefi)) continue;
        options.uefi = firmware == 1;
        WindowsBcdResult prepared;
        error = PrepareWindowsToGoBcd(input.handle(), options, prepared);
    }
    return error;
}

int readGuid(struct fdisk_context* context, size_t number, QUuid& result)
{
    struct fdisk_partition* partition = nullptr;
    int error = -fdisk_get_partition(context, number, &partition);
    if (error == 0)
    {
        const char* id = fdisk_partition_get_uuid(partition);
        result = id == nullptr ? QUuid() : QUuid(QString::fromLatin1(id));
        if (result.isNull()) error = EBADMSG;
    }
    if (partition != nullptr) fdisk_unref_partition(partition);
    return error;
}
}

WindowsToGoImage::~WindowsToGoImage() { close(); }

void WindowsToGoImage::close()
{
    wimlib_free(wim_);
    wim_ = nullptr;
    CloseIsoFileReader(&reader_);
    CloseIsoAnalysisSource(&view_);
    plan = {};
    editions = {};
    architecture = ARCH_UNKNOWN;
    bios = uefi = false;
    archives.clear();
    archive.clear();
    report = {};
}

int WindowsToGoImage::open(const PrepareImageRequest& request, const PrepareIsoSource& source, bool deployment)
{
    close();
    wimError = 0;
    if (!request.windowsToGo || !ValidatePrepareWindowsToGo(request)) return EINVAL;
    int error = CheckWindowsToGoCustomizationSettings(request.windowsFlags, request.windowsUsername);
    GPT_IMAGE_RESULT partition = {};
    if (error == 0) error = PlanFormatPartition(request, partition);
    if (error == 0) error = CheckFormatSettings(request, partition);
    if (error == 0) error = source.check();
    char signature[8];
    if (error == 0)
    {
        ssize_t read;
        do { read = pread(source.retainedSource().fd, signature, sizeof(signature), 0); } while (read < 0 && errno == EINTR);
        if (read < 0) error = errno;
        else if (read != sizeof(signature)) error = EBADMSG;
    }
    const bool standalone = error == 0 && memcmp(signature, "MSWIM\0\0\0", sizeof(signature)) == 0;
    archive = request.windowsInstallPath;
    report.is_windows_img = standalone;
    if (error == 0 && standalone)
    {
        if (!request.windowsInstallPath.isEmpty()) error = EINVAL;
        else wimError = OpenWimFileDescriptor(source.retainedSource().fd, &wim_);
    }
    else if (error == 0)
    {
        ISO_ANALYSIS analysis = {};
        const ISO_ANALYSIS_OPTIONS options = {request.rockRidge, request.joliet, {0, 0}};
        const int status = AnalyzeIsoImageWithSource(source.retainedPath(), request.imageSize, &options, &analysis, &view_);
        if (status == ISO_SCAN_CANCELLED) error = ECANCELED;
        else if (status == ISO_SCAN_ERROR || !analysis.tree_complete || !HAS_WININST(analysis.report)) error = ENOTSUP;
        report = analysis.report;
        for (unsigned i = 0; error == 0 && i < analysis.report.wininst_index && i < MAX_WININST; ++i)
        {
            const char* path = analysis.report.wininst_path[i];
            if (memchr(path, 0, sizeof(analysis.report.wininst_path[i])) == nullptr || strncmp(path, "?:/", 3) != 0) continue;
            archives.append(QString::fromUtf8(path + 2));
        }
        if (error == 0 && !deployment && archive.isEmpty() && !archives.isEmpty()) archive = archives.first();
        const bool selected = archives.contains(archive);
        if (error == 0 && !selected) error = EINVAL;
        if (error == 0)
        {
            const QByteArray path = archive.toUtf8();
            const int opened = OpenIsoFileReader(&reader_, view_.iso, view_.udf, path.constData());
            if (opened != ISO_SCAN_OK) error = opened == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
        }
        if (error == 0) wimError = OpenWimFromIsoReader(&reader_, 0, &wim_);
    }
    if (error == 0) error = nativeWimError(wimError);
    if (error == 0) wimError = ReadWindowsImageEditions(wim_, editions);
    if (error == 0) error = nativeWimError(wimError);
    if (error == 0 && !deployment)
    {
        wimError = ReadWimMetadata(wim_, 1, &plan.metadata);
        error = nativeWimError(wimError);
        if (error == 0) error = source.check();
        if (error != 0) close();
        return error;
    }
    if (error == 0) error = ReadWindowsApplyPlan(wim_, request.windowsEdition, plan);
    if (error == 0 && plan.efsClusterLimit != 0 && request.clusterSize > plan.efsClusterLimit) {
        uprintf("Encrypted sparse streams require NTFS clusters no larger than %u bytes.", plan.efsClusterLimit);
        error = ENOTSUP;
    }
    if (error == 0 && plan.bytes > partition.size) error = ENOSPC;
    if (error == 0)
    {
        uint8_t* bytes = nullptr;
        size_t size = 0;
        wimError = ReadWimFile(wim_, request.windowsEdition, "Windows/Boot/EFI/bootmgr.efi", &bytes, &size);
        error = nativeWimError(wimError);
        if (error == 0) architecture = FindPeArch(bytes, size);
        free(bytes);
        uefi = architecture >= ARCH_X86_32 && architecture <= ARCH_ARM_64;
        if (error == 0 && !uefi) error = ENOTSUP;
    }
    if (error == 0)
    {
        const int pc = wimlib_iterate_dir_tree(wim_, request.windowsEdition, "Windows/Boot/PCAT/bootmgr", 0, regularBootFile, nullptr);
        bios = pc == 0;
        if (pc != 0 && pc != WIMLIB_ERR_PATH_DOES_NOT_EXIST) error = nativeWimError(pc);
        if (request.windowsToGoTarget == TT_BIOS && !bios) error = ENOTSUP;
    }
    if (error == 0)
    {
        const bool ca2023 = request.windowsFlags & UNATTEND_USE_MS2023_BOOTLOADERS;
        const char* manager = ca2023 ? "Windows/Boot/EFI_EX/bootmgfw_EX.efi" : "Windows/Boot/EFI/bootmgfw.efi";
        uint8_t* bytes = nullptr;
        size_t size = 0;
        wimError = ReadWimFile(wim_, request.windowsEdition, manager, &bytes, &size);
        error = nativeWimError(wimError);
        if (error == 0 && FindPeArch(bytes, size) != architecture) error = EBADMSG;
        free(bytes);
        const char* const branches[] = {"Windows/Boot/PCAT", "Windows/Boot/Fonts",
            ca2023 ? "Windows/Boot/EFI_EX" : "Windows/Boot/EFI",
            ca2023 ? "Windows/Boot/Fonts_EX" : "Windows/Boot/Fonts"};
        for (size_t i = bios ? 0 : 2; error == 0 && i < sizeof(branches) / sizeof(branches[0]); ++i)
        {
            wimError = wimlib_iterate_dir_tree(wim_, request.windowsEdition, branches[i],
                WIMLIB_ITERATE_DIR_TREE_FLAG_RECURSIVE, bootTreeEntry, nullptr);
            error = nativeWimError(wimError);
        }
        if (error == 0 && request.windowsFlags != 0)
        {
            const int answer = wimlib_iterate_dir_tree(wim_, request.windowsEdition, "Windows/Panther/unattend.xml", 0, bootTreeEntry, nullptr);
            if (answer == 0) error = EEXIST;
            else if (answer != WIMLIB_ERR_PATH_DOES_NOT_EXIST) error = nativeWimError(answer);
        }
    }
    report.has_bootmgr = bios;
    report.has_bootmgr_efi = uefi;
    report.has_efi = uefi ? (1 | (2 << architecture)) : 0;
    report.wininst_index = 1;
    report.win_version = plan.metadata.win_version;
    if (error == 0)
    {
        const int allowed = GetWindowsToGoUnattendFlags(&report, true) & WindowsToGoCustomizationFlags;
        if (request.windowsFlags & ~allowed) error = ENOTSUP;
        const RUFUS_PARTITION_OPTIONS policy = {BT_IMAGE, FS_NTFS, request.windowsToGoTarget,
            !request.mbr, true, false, request.enhancedInstall, request.oldBiosFixes, 0};
        const quint8 extra = GetExtraPartitions(&report, &policy);
        if (bool(extra & XP_UEFI_NTFS) != request.usesUefiNtfs()) error = EINVAL;
        if (!request.mbr && (extra & (XP_ESP | XP_MSR)) != (XP_ESP | XP_MSR)) error = EINVAL;
    }
    if (error == 0) error = checkTemplate(wim_, request.windowsEdition, bios, uefi);
    if (error == 0) error = source.check();
    if (error != 0) close();
    return error;
}

int ScanWindowsStandaloneImage(const IMAGE_FILE_SOURCE& source, const char* path,
    ISO_ANALYSIS& analysis, WINDOWS_IMAGE_ANALYSIS& windows)
{
    analysis = {};
    windows = {};
    int error = CheckImageFileSource(&source, path);
    WIMStruct* wim = nullptr;
    if (error == 0) windows.wim_error = OpenWimFileDescriptor(source.fd, &wim);
    if (error == 0 && windows.wim_error == 0) windows.wim_error = ReadWimMetadata(wim, 1, &windows.metadata);
    if (error == 0) error = nativeWimError(windows.wim_error);
    uint8_t* bytes = nullptr;
    size_t size = 0;
    if (error == 0) windows.wim_error = ReadWimFile(wim, 1, "Windows/Boot/EFI/bootmgr.efi", &bytes, &size);
    if (error == 0) error = nativeWimError(windows.wim_error);
    const int architecture = error == 0 ? FindPeArch(bytes, size) : ARCH_UNKNOWN;
    free(bytes);
    if (error == 0 && (architecture < ARCH_X86_32 || architecture > ARCH_ARM_64)) error = ENOTSUP;
    if (error == 0)
    {
        auto& report = analysis.report;
        report.is_windows_img = 1;
        report.image_size = quint64(source.info.st_size);
        report.wininst_index = 1;
        report.has_bootmgr_efi = 1;
        report.has_efi = 1 | (2 << architecture);
        report.has_bootmgr = wimlib_iterate_dir_tree(wim, 1, "Windows/Boot/PCAT/bootmgr", 0, regularBootFile, nullptr) == 0;
        report.win_version = windows.metadata.win_version;
        report.wininst_version = windows.metadata.archive.version;
        strcpy(report.label, "Windows");
        analysis.tree_complete = true;
        const bool known = windows.metadata.valid_fields == WIM_VERSION_ALL
            && report.win_version.major != 0 && report.win_version.build != 0;
        windows.state = known ? WINDOWS_IMAGE_KNOWN : WINDOWS_IMAGE_UNKNOWN;
        if (!known) analysis.pending = ISO_PENDING_WINDOWS;
        error = CheckImageFileSource(&source, path);
    }
    wimlib_free(wim);
    if (error != 0)
    {
        analysis = {};
        windows.state = WINDOWS_IMAGE_FAILED;
        return error == ECANCELED || IsIsoScanCancelled() ? ISO_SCAN_CANCELLED : ISO_SCAN_ERROR;
    }
    return analysis.pending == 0 ? ISO_SCAN_OK : ISO_SCAN_PARTIAL;
}

int ReadWindowsToGoBootIdentity(int disk, const PrepareImageRequest& request,
    quint32 signature, WindowsBcdOptions& options)
{
    options = {};
    if (!request.windowsToGo) return EINVAL;
    const off_t position = lseek(disk, 0, SEEK_CUR);
    if (position < 0) return errno;
    GPT_IMAGE_RESULT layout = {};
    int error = PlanFormatPartition(request, layout);
    if (error != 0) return error;
    if (request.mbr)
    {
        const MBR_IMAGE_OPTIONS expected = {request.size, request.logicalSectorSize, 63, request.clusterSize, signature,
            request.device, FS_NTFS, FormatMbrBootCode(request), request.usesUefiNtfs() ? quint64(RUFUS_UEFI_NTFS_BYTES) : 0,
            0, request.oldBiosFixes, true, false, request.physicalSectorSize};
        MBR_IMAGE_RESULT checked = {};
        error = CheckMbrImage(disk, &expected, &checked);
        if (error == 0 && signature == 0) error = EBADMSG;
        if (error == 0)
        {
            options.windows.offset = options.system.offset = layout.offset;
            options.windows.mbrSignature = options.system.mbrSignature = signature;
        }
        return error;
    }
    const GPT_IMAGE_OPTIONS expected = {request.size, request.logicalSectorSize, 63, request.clusterSize, request.device, 0, 0, false, true, request.physicalSectorSize};
    GPT_IMAGE_RESULT checked = {};
    error = CheckGptImagePartition(disk, &expected, &checked);
    if (error != 0) return error;
    struct fdisk_context* context = fdisk_new_context();
    if (context == nullptr) return ENOMEM;
    fdisk_disable_dialogs(context, 1);
    error = -fdisk_save_user_sector_size(context, request.physicalSectorSize, request.logicalSectorSize);
    if (error == 0) error = -fdisk_assign_device_by_fd(context, disk, "Windows To Go target", 1);
    char* id = nullptr;
    if (error == 0) error = -fdisk_get_disklabel_id(context, &id);
    const QUuid diskGuid = id == nullptr ? QUuid() : QUuid(QString::fromLatin1(id));
    free(id);
    if (error == 0 && diskGuid.isNull()) error = EBADMSG;
    WindowsBcdOptions result;
    result.windows.gpt = result.system.gpt = true;
    result.windows.diskGuid = result.system.diskGuid = diskGuid;
    if (error == 0) error = readGuid(context, 2, result.windows.partitionGuid);
    if (error == 0) error = readGuid(context, 0, result.system.partitionGuid);
    fdisk_unref_context(context);
    if (lseek(disk, position, SEEK_SET) != position && error == 0) error = errno;
    if (error == 0) options = result;
    return error;
}
}
