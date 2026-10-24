// SPDX-License-Identifier: GPL-3.0-or-later
#include "ext_format.h"
#include "target_io.h"
#include "rufus_reporting.h"
#include <QFile>
#include <QTemporaryDir>
#include <QScopedPointer>
#include <QtEndian>
#include <ext2fs/ext2fs.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace rufus
{
QString NormalizeExtFormatLabel(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    constexpr int labelBytes = EXT2_LABEL_LEN - 1;
    if (utf8.size() <= labelBytes)
        return text;
    int length = labelBytes;
    // Keep the original byte limit without cutting a UTF-8 character in half.
    while (length > 0 && (static_cast<unsigned char>(utf8[length]) & 0xc0) == 0x80)
        --length;
    return QString::fromUtf8(utf8.constData(), length);
}

namespace
{
constexpr quint64 minimumVolumeBytes = 16 * 1024 * 1024;
constexpr quint64 smallVolumeBytes = 512 * 1024 * 1024;
constexpr quint64 ordinaryVolumeBytes = 4 * 1024 * 1024 * 1024ULL;
constexpr quint64 largeVolumeBytes = 16 * 1024 * 1024 * 1024ULL;
constexpr quint64 maximumBlocks = quint64(UINT32_MAX);
constexpr quint64 superblockOffset = 1024;
constexpr quint32 reservePercent = 5;
constexpr char persistenceFileName[] = "persistence.conf";
constexpr char persistenceContents[] = "/ union\n";
constexpr size_t persistenceContentBytes = sizeof(persistenceContents) - 1;
static_assert(sizeof(ext2_super_block) == 1024);

struct ExtGeometry
{
    quint32 blockSize;
    quint32 inodeSize;
    quint32 bytesPerInode;
};

ExtGeometry geometry(quint64 size, quint32 sectorSize)
{
    if (size < smallVolumeBytes)
        return {sectorSize > 1024 ? sectorSize : 1024, 128, 4096};
    if (size < ordinaryVolumeBytes)
        return {4096, 256, 16384};
    if (size < largeVolumeBytes)
        return {4096, 256, 32768};
    return {4096, 256, 65536};
}

bool hasSparseSuperblock(quint64 group)
{
    if (group <= 1)
        return true;
    for (quint64 base : {3, 5, 7})
    {
        quint64 power = group;
        while (power % base == 0)
            power /= base;
        if (power == 1)
            return true;
    }
    return false;
}

quint64 formattedBlocks(quint64 size, const ExtGeometry& format)
{
    // ext2fs/initialize.c drops a final group smaller than its metadata plus
    // 50 blocks. Track-rounded persistence partitions can hit that boundary.
    // Derive its exact result without allocating filesystem-sized bitmaps.
    const quint64 blocks = size / format.blockSize;
    const quint64 first = format.blockSize == 1024 ? 1 : 0;
    const quint64 perGroup = quint64(format.blockSize) * 8;
    const quint64 groups = (blocks - first + perGroup - 1) / perGroup;
    const quint64 inodes = blocks * format.blockSize / format.bytesPerInode;
    const quint64 perGroupInodes = (inodes + groups - 1) / groups;
    const quint64 inodeBlocks = (perGroupInodes * format.inodeSize + format.blockSize - 1) / format.blockSize;
    const quint64 descriptors = (groups * EXT2_MIN_DESC_SIZE + format.blockSize - 1) / format.blockSize;
    const quint64 overhead = 2 + inodeBlocks + (hasSparseSuperblock(groups - 1) ? 1 + descriptors : 0);
    const quint64 remaining = (blocks - first) % perGroup;
    return remaining != 0 && remaining < overhead + 50 ? blocks - remaining : blocks;
}

int checkTarget(int fd, quint64 device, const QString& program, const ExtFormatOptions& options)
{
    int error = CheckExtFormatOptions(options);
    if (error != 0)
        return error;
    if (!program.startsWith(QChar(u'/')) || program.contains(QChar(u'\0')))
        return EINVAL;
    WRITABLE_TARGET target;
    error = InspectWritableTarget(fd, device, &target);
    if (error != 0)
        return error;
    if (device != 0 && (target.sector_size != options.sectorSize || target.physical_sector_size != options.physicalSectorSize))
        return ENOTSUP;
    if (target.size != options.size)
        return ESTALE;
    return IsRufusJobCancelled() ? ECANCELED : 0;
}

int checkSuperblock(int fd, const ExtFormatOptions& options)
{
    ext2_super_block super = {};
    ssize_t count;
    do {
        count = pread(fd, &super, sizeof(super), superblockOffset);
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        return errno;
    if (count != sizeof(super))
        return EIO;

    const ExtGeometry expected = geometry(options.size, options.sectorSize);
    const quint32 blocks = qFromLittleEndian(super.s_blocks_count);
    const quint32 compat = qFromLittleEndian(super.s_feature_compat);
    const quint32 incompat = qFromLittleEndian(super.s_feature_incompat);
    const quint32 readOnlyCompat = qFromLittleEndian(super.s_feature_ro_compat);
    const quint32 expectedCompat = EXT2_FEATURE_COMPAT_EXT_ATTR | EXT2_FEATURE_COMPAT_DIR_INDEX |
        (options.fileSystem != FS_EXT2 ? EXT3_FEATURE_COMPAT_HAS_JOURNAL : 0);
    const quint32 expectedReadOnlyCompat = EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER |
        EXT2_FEATURE_RO_COMPAT_LARGE_FILE;
    const bool validMagic = qFromLittleEndian(super.s_magic) == EXT2_SUPER_MAGIC;
    const bool supportedRevision = qFromLittleEndian(super.s_rev_level) == EXT2_DYNAMIC_REV;
    const bool clean = qFromLittleEndian(super.s_state) == EXT2_VALID_FS;
    const quint32 blockShift = qFromLittleEndian(super.s_log_block_size);
    const bool blockSizeMatches = blockShift < 3 && (1024u << blockShift) == expected.blockSize;
    const bool inodeSizeMatches = qFromLittleEndian(super.s_inode_size) == expected.inodeSize;
    const quint64 requestedBlocks = options.size / expected.blockSize;
    const quint64 expectedBlocks = formattedBlocks(options.size, expected);
    const bool sizeMatches = blocks == expectedBlocks;
    // initialize.c preserves the original reserved-block ratio after dropping
    // a group, including the initial integer rounding of the requested reserve.
    const quint64 requestedReserve = requestedBlocks * reservePercent / 100;
    const quint64 expectedReserve = requestedReserve * expectedBlocks / requestedBlocks;
    const bool reserveMatches = qFromLittleEndian(super.s_r_blocks_count) == expectedReserve;
    const bool validHeader = validMagic && supportedRevision && clean;
    const bool geometryMatches = blockSizeMatches && inodeSizeMatches && sizeMatches && reserveMatches;
    if (!validHeader || !geometryMatches)
        return EBADMSG;
    const bool featuresMatch = compat == expectedCompat && incompat == (EXT2_FEATURE_INCOMPAT_FILETYPE |
        (options.fileSystem == FS_EXT4 ? EXT3_FEATURE_INCOMPAT_EXTENTS : 0)) &&
        readOnlyCompat == expectedReadOnlyCompat;
    const bool journalMatches = (qFromLittleEndian(super.s_journal_inum) != 0) == (options.fileSystem != FS_EXT2);
    const bool nativeOs = qFromLittleEndian(super.s_creator_os) == EXT2_OS_LINUX;
    const bool errorsContinue = qFromLittleEndian(super.s_errors) == EXT2_ERRORS_CONTINUE;
    const bool policyMatches = featuresMatch && journalMatches && nativeOs && errorsContinue;
    if (!policyMatches)
        return EBADMSG;
    char label[EXT2_LABEL_LEN] = {};
    const QByteArray encoded = options.label.toUtf8();
    memcpy(label, encoded.constData(), size_t(encoded.size()));
    return memcmp(label, super.s_volume_name, sizeof(label)) == 0 ? 0 : EBADMSG;
}
}

int CheckExtFormatOptions(const ExtFormatOptions& options)
{
    if (options.fileSystem != FS_EXT2 && options.fileSystem != FS_EXT3 && options.fileSystem != FS_EXT4)
        return ENOTSUP;
    const bool validSector = options.sectorSize >= 512 && options.sectorSize <= 4096 &&
        (options.sectorSize & (options.sectorSize - 1)) == 0;
    const bool validPhysical = options.physicalSectorSize >= options.sectorSize && options.physicalSectorSize <= 65536 &&
        (options.physicalSectorSize & (options.physicalSectorSize - 1)) == 0;
    if (!validSector || !validPhysical) return EINVAL;
    const bool sizeAligned = options.size % options.sectorSize == 0;
    const bool sizeFits = options.size >= minimumVolumeBytes && options.size <= INT64_MAX;
    if (!sizeAligned || !sizeFits)
        return EINVAL;
    const ExtGeometry expected = geometry(options.size, options.sectorSize);
    const bool blocksFit = options.size / expected.blockSize <= maximumBlocks;
    const QByteArray label = options.label.toUtf8();
    const bool labelFits = label.size() <= EXT2_LABEL_LEN;
    const bool labelValid = !options.label.contains(QChar(u'\0')) &&
        QString::fromUtf8(label) == options.label;
    if (!blocksFit || !labelFits || !labelValid)
        return EINVAL;
    return 0;
}

static int checkPersistenceConf(int fd)
{
    const QByteArray path = "/proc/self/fd/" + QByteArray::number(fd);
    ext2_filsys filesystem = nullptr;
    ext2_file_t file = nullptr;
    ext2_ino_t inode = 0;
    errcode_t error = ext2fs_open(path.constData(), EXT2_FLAG_64BITS, 0, 0, unix_io_manager, &filesystem);
    if (error == 0)
        error = ext2fs_lookup(filesystem, EXT2_ROOT_INO, persistenceFileName, sizeof(persistenceFileName) - 1, nullptr, &inode);
    ext2_inode info = {};
    if (error == 0)
        error = ext2fs_read_inode(filesystem, inode, &info);
    const bool regularFile = (info.i_mode & LINUX_S_IFMT) == LINUX_S_IFREG;
    const bool exactSize = EXT2_I_SIZE(&info) == persistenceContentBytes;
    if (error == 0 && (!regularFile || !exactSize))
        error = EIO;
    if (error == 0)
        error = ext2fs_file_open(filesystem, inode, 0, &file);
    char bytes[sizeof(persistenceContents)] = {};
    unsigned read = 0;
    if (error == 0)
        error = ext2fs_file_read(file, bytes, sizeof(bytes), &read);
    const bool exactContent = read == persistenceContentBytes && memcmp(bytes, persistenceContents, persistenceContentBytes) == 0;
    if (error == 0 && !exactContent)
        error = EIO;
    if (file != nullptr)
    {
        const errcode_t closed = ext2fs_file_close(file);
        if (error == 0) error = closed;
    }
    if (filesystem != nullptr)
    {
        const errcode_t closed = ext2fs_close(filesystem);
        if (error == 0) error = closed;
    }
    if (error != 0)
        uprintf("Cannot verify persistence.conf: filesystem error %ld", long(error));
    return error == 0 ? 0 : EIO;
}

static int createPersistenceSeed(QTemporaryDir& directory)
{
    if (!directory.isValid())
        return EIO;
    QFile file(directory.filePath(QLatin1String(persistenceFileName)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return EIO;
    int error = file.write(persistenceContents, persistenceContentBytes) == qint64(persistenceContentBytes)
        && file.flush() ? 0 : EIO;
    if (error == 0)
    {
        int synced;
        do { synced = fsync(file.handle()); } while (synced != 0 && errno == EINTR);
        if (synced != 0)
            error = errno;
    }
    file.close();
    if (error == 0 && file.error() != QFileDevice::NoError) error = EIO;
    return error;
}

static int formatExt(int fd, quint64 device, const QString& mke2fs,
    const ExtFormatOptions& options, FORMAT_RESULT& result)
{
    result = {};
    result.exit_code = -1;
    int error = checkTarget(fd, device, mke2fs, options);
    if (error != 0)
        return error;
    const ExtGeometry expected = geometry(options.size, options.sectorSize);
    const QByteArray program = QFile::encodeName(mke2fs);
    const QByteArray type = options.fileSystem == FS_EXT4 ? "ext4" : options.fileSystem == FS_EXT3 ? "ext3" : "ext2";
    const QByteArray block = QByteArray::number(expected.blockSize);
    const QByteArray inode = QByteArray::number(expected.inodeSize);
    const QByteArray ratio = QByteArray::number(expected.bytesPerInode);
    const QByteArray reserve = QByteArray::number(reservePercent);
    const QByteArray blocks = QByteArray::number(options.size / expected.blockSize);
    const QByteArray label = options.label.toUtf8();
    const QByteArray features = options.fileSystem == FS_EXT4 ?
        "none,sparse_super,large_file,filetype,dir_index,ext_attr,has_journal,extent" : options.fileSystem == FS_EXT3 ?
        "none,sparse_super,large_file,filetype,dir_index,ext_attr,has_journal" :
        "none,sparse_super,large_file,filetype,dir_index,ext_attr";
    const char* extended = options.fileSystem != FS_EXT2 ?
        (options.quickFormat ? "nodiscard,lazy_itable_init=0,lazy_journal_init=1" : "nodiscard,lazy_itable_init=0,lazy_journal_init=0") : "nodiscard,lazy_itable_init=0";
    const QByteArray target = QByteArray("/proc/self/fd/") + QByteArray::number(FORMAT_TARGET_FD);
    // A root helper uses a root-owned parent; caller-controlled temporary
    // directories must never redirect privileged mke2fs population reads.
    const QString seedTemplate = getuid() == 0 ? QStringLiteral("/run/rufus-persist-XXXXXX")
        : QStringLiteral("/tmp/rufus-persist-XXXXXX");
    QScopedPointer<QTemporaryDir> seed;
    if (options.persistenceConf) {
        seed.reset(new QTemporaryDir(seedTemplate));
        seed->setAutoRemove(false);
        error = createPersistenceSeed(*seed);
    }
    const QByteArray seedPath = seed ? QFile::encodeName(seed->path()) : QByteArray();
    const char* arguments[] = {program.constData(), "-q", "-F", "-t", type.constData(),
        "-b", block.constData(), "-I", inode.constData(), "-i", ratio.constData(),
        "-m", reserve.constData(), "-e", "continue", "-O", features.constData(), "-E", extended,
        "-L", label.constData(), seed ? "-d" : target.constData(), seed ? seedPath.constData() : blocks.constData(),
        seed ? target.constData() : nullptr, seed ? blocks.constData() : nullptr, nullptr};
    if (error == 0) error = options.quickFormat ? RunFormatCommand(fd, const_cast<char* const*>(arguments), &result)
        : RunFullFormatCommand(fd, const_cast<char* const*>(arguments), &result);
    if (seed && !seed->remove() && (error == 0 || error == ECANCELED)) error = EIO;
    if (result.diagnostics[0])
        uprintf("mke2fs: %s", result.diagnostics);
    if (error == 0)
        error = checkTarget(fd, device, mke2fs, options);
    if (error == 0)
        error = checkSuperblock(fd, options);
    if (error == 0 && options.persistenceConf)
        error = checkPersistenceConf(fd);
    if (error == 0)
        result.filesystem_bytes = options.size;
    return error;
}

int FormatExtImage(int fd, const QString& mke2fs, const ExtFormatOptions& options, FORMAT_RESULT& result)
{
    return formatExt(fd, 0, mke2fs, options, result);
}

int FormatExtDevice(int fd, quint64 device, const QString& mke2fs,
    const ExtFormatOptions& options, FORMAT_RESULT& result)
{
    result = {};
    result.exit_code = -1;
    if (device == 0)
        return EINVAL;
    return formatExt(fd, device, mke2fs, options, result);
}
}
