// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "device_selection.h"
#include "fat_format.h"
#include "ntfs_format.h"
#include "ntfs_boot.h"
#include "exfat_format.h"
#include "ext_format.h"
#include "udf_format.h"
#include "partition_image.h"
#include "windows_apply.h"
#include "qt/prepare_image_protocol.h"
#include <QDBusConnection>
#include <QProcess>
#include <QVector>

namespace rufus
{
class PrepareIsoSource;
// One native target for the helper's preparation. Owns the exclusive
// whole-disk descriptor. FAT/exFAT/ext release it for mkfs's claim; NTFS retains it.
class PrepareDeviceTarget
{
public:
    enum class Table { Gpt, Mbr, Sfd, Raw };
    // The table must match the caller's verified partition creation. Never
    // infer MBR from an unreadable/damaged GPT. Existing jobs retain GPT.
    // Raw acquires/checks the disk. Only the explicit optimized FFU methods may
    // refresh partitions or mount a restored NTFS volume for finalization.
    explicit PrepareDeviceTarget(const PrepareImageRequest& request, Table table = Table::Gpt);
    ~PrepareDeviceTarget();
    PrepareDeviceTarget(const PrepareDeviceTarget&) = delete;
    PrepareDeviceTarget& operator=(const PrepareDeviceTarget&) = delete;
    // Mounted or natively protected acquisition needs the authenticated caller and
    // retained source (nullptr for source-free jobs). Unmounted removable
    // native tests need neither.
    int open(uid_t caller = 0, const PrepareIsoSource* source = nullptr);
    int open(uid_t caller, const PrepareIsoSource* source, const PrepareIsoSource* archive);
    int check();
    // Raw jobs close their exclusive claim and caller-home descriptor explicitly
    // before publishing an outcome. Close once even after EINTR; return cleanup errno.
    int finishRaw();
    // Checked final close for complete native jobs; require no live mount.
    int finish();
    int descriptor() const { return fd_; }
    // SFD is non-bootable FAT16/FAT32/NTFS/exFAT only at this checkpoint. The caller first
    // runs PrepareSfdTarget while holding the disk; format refreshes kernel
    // partitions and validates the whole volume after native mkfs handoff.
    int format(const FAT32_FORMAT_OPTIONS& options, const char* mkfs, FAT32_FORMAT_RESULT& result);
    int formatNtfs(const GPT_IMAGE_RESULT& layout, const NtfsFormatOptions& options,
        const QString& mkfs, NtfsFormatResult& result);
    int installNtfsBoot(const NtfsFormatOptions& options, NtfsBootResult& result);
    int applyWindowsImage(WIMStruct* wim, int image, const PrepareIsoSource& source,
        const GPT_IMAGE_RESULT& layout, WindowsApplyResult& result);
    int formatWindowsEsp(const GPT_IMAGE_RESULT& layout, const char* mkfs, FAT32_FORMAT_RESULT& result);
    int mountWindowsEsp(const GPT_IMAGE_RESULT& layout);
    int formatExfat(const ExfatFormatOptions& options, const QString& mkfs, FORMAT_RESULT& result);
    int formatExt(const GPT_IMAGE_RESULT& layout, const ExtFormatOptions& options,
        const QString& mke2fs, FORMAT_RESULT& result);
    int formatUdf(const GPT_IMAGE_RESULT& layout, const UdfFormatOptions& options,
        const QString& mkfs, FORMAT_RESULT& result);
    int formatPersistence(const GPT_IMAGE_RESULT& layout, bool casper, const QString& mke2fs, FORMAT_RESULT& result);
    int checkMountSupport();
    void requireWindowsLinks(bool required) { windowsLinks_ = required; }
    int mount(quint64 offset, quint64 size);
    // Always call after a mount attempt, including failure/cancellation. Flush,
    // close, unmount, remove the empty mountpoint, and reacquire the disk.
    int finishMount(int& syncError);
    const char* mountPath() const { return mountPath_.constData(); }
    // Borrow only after a successful mount; finishMount owns closing it.
    int mountDescriptor() const { return directory_; }
    int partitionDescriptor() const { return partition_; }
    int checkFfuMountSupport();
    int prepareFfuPartitions();
    int openFfuPartition(quint32 number, quint64 offset, quint64 size, quint64& device);
    int mountFfuPartition(quint32 number, quint64 offset, quint64 size);
    int checkFfuPartitionTable();

private:
    int checkMountSupport(int filesystem);
    int mountPartition(quint32 number, quint64 offset, quint64 size, int filesystem);
    int saveFfuPartitionTable();
    int checkSnapshot();
    int prepareTargetMounts(uid_t caller, const PrepareIsoSource* source, const PrepareIsoSource* archive);
    int waitForUnmountSnapshot();
    int checkDescriptor(int fd, quint64 device, quint64 size);
    int openPartition(quint32 number, quint64 offset, quint64 size, quint64& device);
    int checkNoPartitions();
    int checkSfdPartitionEntries();
    int checkSfdFormat(const FAT32_FORMAT_OPTIONS& options);
    int savePartitionTable();
    int checkSavedPartitionTable();
    int checkMbrIdentity();
    int reacquire();
    int beginFormat(quint64 offset, quint64 size, int& partition, quint64& partitionDevice, quint32 number = 1);
    int finishFormat(int partition, int error);
    int mountNtfs3g(quint64 device);
    int finishNtfs3g();

    const PrepareImageRequest& request_;
    const Table table_;
    int fd_ = -1;
    int callerHome_ = -1;
    int partition_ = -1;
    int directory_ = -1;
    bool mounted_ = false;
    bool windowsLinks_ = false;
    bool cleanupAttempted_ = false;
    QByteArray mountPath_;
    QByteArray primary_;
    QByteArray backup_;
    struct TableSnapshot { quint64 offset; QByteArray bytes; };
    QVector<TableSnapshot> ffuTable_;
    bool kernelNtfs_ = false;
    QProcess ntfs3g_;
};
}
