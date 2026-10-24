// SPDX-License-Identifier: GPL-3.0-or-later
// FAT view/active-MBR policy follows src/embedded_efi.c (Pete Batard, GPLv3+).
#include "embedded_efi_files.h"
extern "C" {
#include "syslinux/libfat/libfat.h"
}
#include <QByteArray>
#include <QSet>
#include <QString>
#include <QVector>
#include <QtEndian>
#include <algorithm>
#include <cerrno>
#include <cstring>
namespace {
constexpr quint64 maximumFileBytes = 64 * 1024 * 1024, maximumTreeBytes = 512 * 1024 * 1024;
struct Reader {
    ISO_FILE_READER image = {};
    libfat_filesystem* fat = nullptr;
    quint64 base = 0, length = 0, total = 0;
    unsigned entries = 0;
    int error = 0;
    QSet<quint64> directories;
    QSet<QByteArray> paths;
    int (*visit)(const EMBEDDED_EFI_FILE*, void*);
    void* context;
    ~Reader() { if (fat) libfat_close(fat); CloseIsoFileReader(&image); }
};
struct FileReader { Reader* owner; QVector<quint64> sectors; };
int readSector(intptr_t opaque, void* output, size_t size, libfat_sector_t sector) {
    auto& r = *reinterpret_cast<Reader*>(opaque);
    if (IsIsoScanCancelled()) { r.error = ECANCELED; return 0; }
    if (size == 0 || size > r.length || sector > (r.length - size) / size) { r.error = EBADMSG; return 0; }
    const int status = ReadIsoFileRange(&r.image, r.base + sector * size, output, size);
    if (status != ISO_SCAN_OK) { r.error = status == ISO_SCAN_CANCELLED ? ECANCELED : EIO; return 0; }
    return int(size);
}
int prepare(Reader& r) {
    unsigned char boot[512];
    if (!readSector(intptr_t(&r), boot, sizeof(boot), 0)) return r.error;
    const unsigned type = boot[450];
    const bool fatPartition = type == 1 || type == 4 || type == 6 || type == 0xb || type == 0xc || type == 0xe;
    if (boot[510] == 0x55 && boot[511] == 0xaa && boot[446] == 0x80 && fatPartition) {
        const quint64 start = qFromLittleEndian<quint32>(boot + 454), count = qFromLittleEndian<quint32>(boot + 458);
        if (start == 0 || start % 4 || count == 0 || start + count > r.image.size / 512) return EBADMSG;
        r.base = start * 512; r.length = count * 512;
        if (!readSector(intptr_t(&r), boot, sizeof(boot), 0)) return r.error;
    }
    const unsigned sector = qFromLittleEndian<quint16>(boot + 11);
    if (boot[510] != 0x55 || boot[511] != 0xaa || sector < 512 || sector > 4096 || (sector & (sector - 1))) return EBADMSG;
    quint64 count = qFromLittleEndian<quint16>(boot + 19);
    if (!count) count = qFromLittleEndian<quint32>(boot + 32);
    if (!count || count > r.length / sector) return EBADMSG;
    r.length = count * sector;
    LIBFAT_SECTOR_SIZE = sector; LIBFAT_SECTOR_MASK = sector - 1; LIBFAT_SECTOR_SHIFT = 0;
    for (unsigned value = sector; value > 1; value >>= 1) ++LIBFAT_SECTOR_SHIFT;
    r.fat = libfat_open(readSector, intptr_t(&r));
    return r.fat ? 0 : r.error ? r.error : EBADMSG;
}
int fileSectors(Reader& r, quint32 cluster, quint64 size, FileReader& file) {
    if (!size) return 0;
    if (cluster < 2 || size > maximumFileBytes) return EFBIG;
    const quint64 count = (size + LIBFAT_SECTOR_SIZE - 1) / LIBFAT_SECTOR_SIZE;
    quint64 sector = libfat_clustertosector(r.fat, int32_t(cluster)); QSet<quint64> seen;
    for (quint64 i = 0; i < count; ++i) {
        if (IsIsoScanCancelled()) return ECANCELED;
        if (!sector || sector == UINT64_MAX || seen.contains(sector)) return EBADMSG;
        if (sector >= r.length / LIBFAT_SECTOR_SIZE) return EBADMSG;
        seen.insert(sector); file.sectors.append(sector);
        if (i + 1 < count) sector = libfat_nextsector(r.fat, sector);
    }
    return r.error;
}
int walk(Reader& r, quint32 cluster, const QByteArray& parent, int depth) {
    if (depth > 32 || r.directories.contains(cluster)) return ELOOP;
    r.directories.insert(cluster);
    quint64 sector = libfat_clustertosector(r.fat, int32_t(cluster)); QSet<quint64> seen;
    char16_t longName[260] = {}; int sequence = 0, checksum = -1;
    while (sector && sector != UINT64_MAX) {
        if (seen.contains(sector) || seen.size() >= 131072) return ELOOP;
        seen.insert(sector);
        const void* data = libfat_get_sector(r.fat, sector);
        if (!data) return r.error ? r.error : EIO;
        const QByteArray block(static_cast<const char*>(data), LIBFAT_SECTOR_SIZE);
        for (int offset = 0; offset < block.size(); offset += 32) {
            if (IsIsoScanCancelled()) return ECANCELED;
            const auto* e = reinterpret_cast<const uchar*>(block.constData() + offset);
            if (!e[0]) return 0;
            if (e[0] == 0xe5) { sequence = 0; continue; }
            if (e[11] == 0xf) {
                const int index = e[0] & 0x1f;
                if (e[0] & 0x40) { memset(longName, 0, sizeof(longName)); sequence = index; checksum = e[13]; }
                if (index < 1 || index > 20 || index != sequence || e[13] != checksum || e[12] || e[26] || e[27]) return EBADMSG;
                const int positions[] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
                for (int i = 0; i < 13; ++i) longName[(index - 1) * 13 + i] = qFromLittleEndian<quint16>(e + positions[i]);
                --sequence; continue;
            }
            if (e[11] & 8 || e[0] == '.') { sequence = 0; checksum = -1; continue; }
            QByteArray name;
            if (checksum >= 0) {
                int actual = 0; for (int i = 0; i < 11; ++i) actual = ((actual & 1) ? 128 : 0) + (actual >> 1) + e[i], actual &= 255;
                if (sequence != 0 || actual != checksum) return EBADMSG;
                int length = 0; while (length < 260 && longName[length] && longName[length] != 0xffff) ++length;
                if (length == 260) return ENAMETOOLONG;
                const QString unicode = QString::fromUtf16(longName, length);
                if (unicode.contains(QChar::ReplacementCharacter)) return EILSEQ;
                name = unicode.toUtf8();
            } else {
                name = QByteArray(reinterpret_cast<const char*>(e), 8).trimmed();
                const QByteArray extension = QByteArray(reinterpret_cast<const char*>(e + 8), 3).trimmed();
                if (!extension.isEmpty()) name += '.' + extension;
            }
            sequence = 0; checksum = -1;
            if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.contains('\\') || name.contains(':')) return EINVAL;
            const QByteArray path = parent + '/' + name;
            if (path.size() >= ISO_SCAN_PATH_MAX || ++r.entries > 65536) return E2BIG;
            if (r.paths.contains(path.toLower())) return EEXIST;
            r.paths.insert(path.toLower());
            const quint32 child = qFromLittleEndian<quint16>(e + 26) | (quint32(qFromLittleEndian<quint16>(e + 20)) << 16);
            const bool directory = e[11] & 0x10;
            const quint64 size = directory ? 0 : qFromLittleEndian<quint32>(e + 28);
            if (size > maximumTreeBytes - r.total) return EFBIG;
            r.total += size;
            FileReader file{&r,{}};
            int error = directory ? 0 : fileSectors(r, child, size, file);
            const EMBEDDED_EFI_FILE entry{path.constData(),size,directory,&file};
            if (!error) error = r.visit(&entry, r.context);
            if (!error && directory) error = walk(r, child, path, depth + 1);
            if (error) return error;
        }
        sector = libfat_nextsector(r.fat, sector);
    }
    return sector == UINT64_MAX ? EBADMSG : 0;
}
}
extern "C" int ReadEmbeddedEfiRange(const EMBEDDED_EFI_FILE* file, uint64_t offset, void* output, size_t size) {
    if (!file || file->directory || !file->reader || offset > file->size || size > file->size - offset) return EINVAL;
    auto& reader = *static_cast<FileReader*>(file->reader); auto& r = *reader.owner;
    auto* destination = static_cast<char*>(output);
    while (size) {
        if (IsIsoScanCancelled()) return ECANCELED;
        const quint64 index = offset / LIBFAT_SECTOR_SIZE; const unsigned skip = unsigned(offset % LIBFAT_SECTOR_SIZE);
        if (index >= quint64(reader.sectors.size())) return EBADMSG;
        const auto* sector = static_cast<const char*>(libfat_get_sector(r.fat, reader.sectors[qsizetype(index)]));
        if (!sector) return r.error ? r.error : EIO;
        const size_t count = std::min(size, size_t(LIBFAT_SECTOR_SIZE - skip));
        memcpy(destination, sector + skip, count); destination += count; offset += count; size -= count;
    }
    return 0;
}
extern "C" int WalkEmbeddedEfiFiles(iso9660_t* iso, udf_t* udf, const char* image, int (*visit)(const EMBEDDED_EFI_FILE*,void*), void* context) {
    if ((iso == nullptr) == (udf == nullptr) || !image || !visit) return EINVAL;
    const quint32 savedSize = LIBFAT_SECTOR_SIZE, savedMask = LIBFAT_SECTOR_MASK, savedShift = LIBFAT_SECTOR_SHIFT;
    int error;
    {
        Reader r; r.visit = visit; r.context = context;
        const int status = OpenIsoFileReader(&r.image,iso,udf,image);
        error = status == ISO_SCAN_OK ? 0 : status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
        r.length = r.image.size;
        if (!error) error = prepare(r);
        if (!error) error = walk(r,0,{},0);
    }
    LIBFAT_SECTOR_SIZE = savedSize; LIBFAT_SECTOR_MASK = savedMask; LIBFAT_SECTOR_SHIFT = savedShift;
    return error;
}
extern "C" int EmbeddedEfiPrecedence(iso9660_t* iso, udf_t* udf, const RUFUS_IMG_REPORT* report, const EMBEDDED_EFI_FILE* file, bool* copy) {
    *copy = true;
    const bool repair = (report->has_efi & 0x4000) && strcasecmp(file->path,"/efi/boot/bootx64.efi") == 0;
    if (iso) {
        iso9660_stat_t* entry = iso9660_ifs_stat_translate(iso,file->path);
        if (entry) { *copy = repair; const bool conflict = (entry->type == iso9660_stat_t::_STAT_DIR) != file->directory; iso9660_stat_free(entry); if (conflict) return EEXIST; }
    } else {
        udf_dirent_t* root = udf_get_root(udf,true,0);
        udf_dirent_t* entry = root ? udf_fopen(root,file->path) : nullptr;
        udf_dirent_free(root);
        if (entry) { *copy = repair; const bool conflict = bool(udf_is_dir(entry)) != file->directory; udf_dirent_free(entry); if (conflict) return EEXIST; }
    }
    return 0;
}
