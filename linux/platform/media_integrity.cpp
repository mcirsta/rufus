// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_integrity.h"
#include "hash_native.h"
#include "image_report.h"
#include "image_file.h"
#include "iso_file.h"
#include "rufus_reporting.h"
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void initializeMediaValidators() { Q_INIT_RESOURCE(media_integrity); }

namespace rufus
{
namespace
{
constexpr quint64 maximumManifestBytes = 64 * 1024 * 1024;
constexpr unsigned maximumManifestFiles = 262144;
constexpr int maximumPathBytes = 4095;
constexpr size_t hashBufferBytes = 64 * 1024;
const char* const manifestNames[] = {nullptr, "md5sum.txt", "MD5SUMS"};

int syncFile(int descriptor, int& syncError)
{
    int status;
    do { status = fsync(descriptor); } while (status != 0 && errno == EINTR);
    if (status == 0) return 0;
    if (syncError == 0) syncError = errno;
    return errno;
}

int verifyBytes(int descriptor, const QByteArray& expected)
{
    unsigned char buffer[hashBufferBytes];
    qsizetype offset = 0;
    while (offset < expected.size()) {
        if (IsRufusJobCancelled()) return ECANCELED;
        const size_t amount = size_t(qMin(qsizetype(sizeof(buffer)), expected.size() - offset));
        const ssize_t count = pread(descriptor, buffer, amount, off_t(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count < 0 ? errno : EIO;
        if (memcmp(buffer, expected.constData() + offset, size_t(count)) != 0) return EBADMSG;
        offset += count;
    }
    return 0;
}

bool validPath(const QByteArray& path)
{
    const bool bounded = !path.isEmpty() && path.size() <= maximumPathBytes;
    if (!bounded || path.startsWith('/') || path.contains('\0') || path.contains('\n') || path.contains('\r')) return false;
    if (QString::fromUtf8(path).toUtf8() != path) return false;
    const auto parts = path.split('/');
    for (const auto& part : parts) if (part.isEmpty() || part == "." || part == "..") return false;
    return true;
}

QByteArray relativePath(const char* path)
{
    if (path == nullptr) return {};
    QByteArray result(path);
    if (result.startsWith("/")) result.remove(0, 1); // Extractor's rooted output path.
    if (result.startsWith("./")) result.remove(0, 2);
    return validPath(result) ? result : QByteArray();
}

// Existing checksum files and EFI paths use Windows case-insensitive names.
// Resolve each component without allowing a symlink or an ambiguous alias.
int childName(int parent, const QByteArray& requested, QByteArray& actual)
{
    const int descriptor = openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return errno;
    DIR* directory = fdopendir(descriptor);
    if (directory == nullptr) { const int error = errno; ::close(descriptor); return error; }
    actual.clear(); unsigned entries = 0; int error = 0;
    for (;;) {
        errno = 0;
        const dirent* entry = readdir(directory);
        if (entry == nullptr) { if (errno != 0) error = errno; break; }
        if (++entries > maximumManifestFiles) { error = EFBIG; break; }
        const QByteArray name(entry->d_name);
        if (name.compare(requested, Qt::CaseInsensitive) != 0) continue;
        if (!actual.isEmpty()) { error = EEXIST; break; }
        actual = name;
    }
    if (closedir(directory) != 0 && error == 0) error = errno;
    return error != 0 ? error : actual.isEmpty() ? ENOENT : 0;
}

int openRelative(int root, const QByteArray& path, int& descriptor)
{
    descriptor = -1;
    if (!validPath(path)) return EINVAL;
    int parent = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) return errno;
    const auto parts = path.split('/');
    for (qsizetype index = 0; index < parts.size(); ++index) {
        QByteArray actual;
        int error = childName(parent, parts[index], actual);
        const bool directory = index + 1 < parts.size();
        int child = -1;
        if (error == 0) {
            child = openat(parent, actual.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (directory ? O_DIRECTORY : O_NONBLOCK));
            if (child < 0) error = errno;
        }
        if (::close(parent) != 0 && error == 0) error = errno;
        if (error != 0) { if (child >= 0) ::close(child); return error; }
        parent = child;
    }
    descriptor = parent;
    return 0;
}

bool sameFile(const struct stat& before, const struct stat& after)
{
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_size == after.st_size
        && before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec
        && before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

int readRegular(int root, const QByteArray& path, QByteArray& bytes)
{
    bytes.clear(); int descriptor = -1;
    int error = openRelative(root, path, descriptor);
    if (error != 0) return error;
    struct stat before = {}, after = {};
    if (fstat(descriptor, &before) != 0) error = errno;
    const bool regular = S_ISREG(before.st_mode) && before.st_nlink == 1 && before.st_size >= 0;
    if (error == 0 && !regular) error = ENOTSUP;
    if (error == 0 && quint64(before.st_size) > maximumManifestBytes) error = EFBIG;
    if (error == 0) bytes.resize(qsizetype(before.st_size));
    qsizetype done = 0;
    while (error == 0 && done < bytes.size()) {
        if (IsRufusJobCancelled()) { error = ECANCELED; break; }
        const ssize_t count = pread(descriptor, bytes.data() + done, size_t(bytes.size() - done), done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) error = count < 0 ? errno : EIO;
        else done += count;
    }
    if (error == 0 && fstat(descriptor, &after) != 0) error = errno;
    if (error == 0 && !sameFile(before, after)) error = ESTALE;
    if (::close(descriptor) != 0 && error == 0) error = errno;
    if (error != 0) bytes.clear();
    return error;
}

int hashRegular(int root, const QByteArray& path, QByteArray& digest, quint64* bytes = nullptr)
{
    int descriptor = -1;
    int error = openRelative(root, path, descriptor);
    if (error != 0) return error;
    struct stat before = {}, after = {};
    if (fstat(descriptor, &before) != 0) error = errno;
    const bool regular = S_ISREG(before.st_mode) && before.st_nlink == 1 && before.st_size >= 0;
    if (error == 0 && !regular) error = ENOTSUP;
    HASH_CONTEXT hash = {};
    hash_init[HASH_MD5](&hash);
    unsigned char buffer[hashBufferBytes]; quint64 offset = 0;
    while (error == 0 && offset < quint64(before.st_size)) {
        if (IsRufusJobCancelled()) { error = ECANCELED; break; }
        const size_t amount = size_t(qMin(quint64(sizeof(buffer)), quint64(before.st_size) - offset));
        const ssize_t count = pread(descriptor, buffer, amount, off_t(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) error = count < 0 ? errno : EIO;
        else { hash_write[HASH_MD5](&hash, buffer, size_t(count)); offset += quint64(count); }
    }
    if (error == 0 && fstat(descriptor, &after) != 0) error = errno;
    if (error == 0 && !sameFile(before, after)) error = ESTALE;
    if (error == 0) { hash_final[HASH_MD5](&hash); if (bytes) *bytes = offset; digest = QByteArray(reinterpret_cast<const char*>(hash.buf), MD5_HASHSIZE).toHex(); }
    if (::close(descriptor) != 0 && error == 0) error = errno;
    return error;
}

QString key(const QByteArray& path) { return QString::fromUtf8(path).toCaseFolded(); }

bool checksumLine(const QByteArray& line, QByteArray& path)
{
    if (line.size() < 35 || line[32] != ' ' || (line[33] != ' ' && line[33] != '*')) return false;
    for (int index = 0; index < 32; ++index) {
        const char value = line[index];
        const bool hexadecimal = (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
        if (!hexadecimal) return false;
    }
    path = line.mid(34);
    if (path.endsWith('\n')) path.chop(1);
    if (path.endsWith('\r')) path.chop(1);
    if (path.startsWith("./")) path.remove(0, 2);
    return validPath(path);
}

struct IntegritySourcePlan
{
    bool runtime = false, generate = false;
    unsigned splitParts = 0, files = 0;
    quint64 generated = 64, manifestSize = 0, payloadBytes = 0;
    int error = 0;
    QByteArray manifestPath;
    QHash<QByteArray,quint64> validators;
};

int planIntegrityEntry(const ISO_TREE_ENTRY* entry, void* opaque)
{
    auto* plan = static_cast<IntegritySourcePlan*>(opaque);
    if (entry->kind != ISO_TREE_FILE) return ISO_SCAN_OK;
    const quint64 size = entry->iso != nullptr ? entry->iso->total_size : udf_get_file_length(entry->udf);
    QByteArray path(entry->path); if (path.startsWith('/')) path.remove(0, 1);
    if (!validPath(path)) plan->error = EINVAL;
    const bool checksum = path.compare("md5sum.txt", Qt::CaseInsensitive) == 0 || path.compare("MD5SUMS", Qt::CaseInsensitive) == 0;
    if (checksum && size > maximumManifestBytes) plan->error = EFBIG;
    if (path.compare(plan->manifestPath, Qt::CaseInsensitive) == 0) plan->manifestSize = size;
    if (plan->runtime) {
        const QByteArray lower = path.toLower();
        if (plan->validators.contains(lower)) plan->payloadBytes += plan->validators.value(lower);
        for (auto item = plan->validators.cbegin(); item != plan->validators.cend(); ++item) {
            const QByteArray backup = item.key().left(item.key().size() - 4) + "_original.efi";
            if (lower == backup) { plan->error = EEXIST; break; }
        }
    }
    if (plan->generate && !checksum && path.compare("ldlinux.sys", Qt::CaseInsensitive) != 0) {
        const bool split = plan->splitParts != 0 && path.compare("sources/install.wim", Qt::CaseInsensitive) == 0;
        const quint64 count = split ? plan->splitParts : 1;
        const quint64 lineBytes = split ? 35 + maximumPathBytes : quint64(path.size()) + 35;
        if (count > maximumManifestFiles - plan->files || count * lineBytes > maximumManifestBytes - plan->generated)
            plan->error = EFBIG;
        else { plan->files += unsigned(count); plan->generated += count * lineBytes; }
    }
    return plan->error == 0 ? ISO_SCAN_OK : ISO_SCAN_ERROR;
}
}

int ReadMediaValidator(int architecture, QByteArray& bytes)
{
    bytes.clear();
    if (architecture <= ARCH_UNKNOWN || architecture >= ARCH_MAX) return EINVAL;
    static const bool initialized = (initializeMediaValidators(), true);
    Q_UNUSED(initialized);
    const QString name = QStringLiteral(":/rufus/md5/boot%1.efi").arg(QString::fromLatin1(efi_archname[architecture]));
    QFile file(name);
    if (!file.open(QIODevice::ReadOnly)) return ENOENT;
    bytes = file.readAll();
    if (bytes.isEmpty() || bytes.size() > 1024 * 1024) { bytes.clear(); return EBADMSG; }
    return 0;
}

MediaIntegrity::~MediaIntegrity() { close(); }

int MediaIntegrity::begin(int root, unsigned manifestKind, bool runtime)
{
    if (active()) return EBUSY;
    if (manifestKind > 2) return EINVAL;
    const int initialized = InitializeNativeHashes();
    if (initialized != 0) return initialized;
    struct stat directory = {};
    if (fstat(root, &directory) != 0) return errno;
    if (!S_ISDIR(directory.st_mode)) return ENOTDIR;
    root_ = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_ < 0) return errno;
    kind_ = manifestKind; runtime_ = runtime; syncError_ = 0;
    generatedBytes_ = files_ = bytes_ = modifiedNameBytes_ = 0; modified_.clear(); relocations_.clear(); aliases_.clear(); replaced_.clear(); replacementLines_.clear();
    if (runtime && manifestKind != 1) {
        generated_ = std::make_unique<QTemporaryFile>(QStringLiteral("/proc/self/fd/%1/.rufus-md5-XXXXXX").arg(root_));
        if (!generated_->open()) { close(); return EIO; }
    }
    return 0;
}

int MediaIntegrity::copied(const char* path, quint64 size, const unsigned char* md5)
{
    if (!active()) return EINVAL;
    if (!generated_) return 0;
    const QByteArray relative = relativePath(path);
    if (relative.isEmpty() || md5 == nullptr) return EINVAL;
    const bool checksumFile = relative.compare("md5sum.txt", Qt::CaseInsensitive) == 0
        || relative.compare("MD5SUMS", Qt::CaseInsensitive) == 0;
    if (checksumFile) return 0;
    if (files_ >= maximumManifestFiles || size > UINT64_MAX - bytes_) return EFBIG;
    const QByteArray line = QByteArray(reinterpret_cast<const char*>(md5), MD5_HASHSIZE).toHex() + "  ./" + relative + '\n';
    if (quint64(line.size()) > maximumManifestBytes - generatedBytes_) return EFBIG;
    if (IsRufusJobCancelled()) return ECANCELED;
    if (generated_->write(line) != line.size()) return EIO;
    generatedBytes_ += quint64(line.size()); ++files_; bytes_ += size;
    return 0;
}

int MediaIntegrity::modified(const char* path)
{
    if (!active()) return EINVAL;
    const QByteArray relative = relativePath(path);
    if (relative.isEmpty()) return EINVAL;
    const QString folded = key(relative);
    if (modified_.contains(folded)) return 0;
    if (modified_.size() >= maximumManifestFiles || quint64(relative.size()) > maximumManifestBytes - modifiedNameBytes_) return EFBIG;
    modified_.insert(folded); modifiedNameBytes_ += quint64(relative.size());
    return 0;
}

int MediaIntegrity::relocated(const char* original, const char* destination)
{
    if (!active()) return EINVAL;
    const QByteArray before = relativePath(original), after = relativePath(destination);
    if (before.isEmpty() || after.isEmpty() || relocations_.size() >= 32) return EINVAL;
    const QString prefix = key(before);
    relocations_.insert(prefix,after);
    QSet<QString> updated;
    for (const QString& name : modified_)
        updated.insert(name.startsWith(prefix + '/') ? key(after) + name.mid(prefix.size()) : name);
    modified_.swap(updated);
    QMap<QString,QByteArray> aliases;
    for (auto alias = aliases_.cbegin(); alias != aliases_.cend(); ++alias) {
        const QString name = alias.key().startsWith(prefix + '/') ? key(after) + alias.key().mid(prefix.size()) : alias.key();
        const QByteArray target = key(alias.value()).startsWith(prefix + '/') ? after + alias.value().mid(before.size()) : alias.value();
        aliases.insert(name,target);
    }
    aliases_.swap(aliases);
    return 0;
}

int MediaIntegrity::linked(const char* path, const char* target)
{
    if (!active()) return EINVAL;
    const QByteArray relative = relativePath(path), resolved = relativePath(target);
    if (relative.isEmpty() || resolved.isEmpty()) return EINVAL;
    if (aliases_.size() >= int(maximumManifestFiles)) return EFBIG;
    QByteArray digest; quint64 bytes = 0;
    int error = hashRegular(root_,resolved,digest,&bytes);
    if (error) return error;
    aliases_.insert(key(relative),resolved);
    error = modified(path);
    if (error) return error;
    const QByteArray raw = QByteArray::fromHex(digest);
    return copied(path,bytes,reinterpret_cast<const unsigned char*>(raw.constData()));
}

int MediaIntegrity::splitPart(const char* original, int stage, const char* name)
{
    if (!active()) return EINVAL;
    const QByteArray source = relativePath(original), part = relativePath(name);
    if (source.compare("sources/install.wim", Qt::CaseInsensitive) != 0 || part.isEmpty() || part.contains('/')) return EINVAL;
    const QByteArray path = source.left(source.lastIndexOf('/') + 1) + part;
    QByteArray digest;
    int error = hashRegular(stage, part, digest);
    struct stat info = {};
    if (error == 0 && fstatat(stage, part.constData(), &info, AT_SYMLINK_NOFOLLOW) != 0) error = errno;
    if (error != 0) return error;
    const QByteArray line = digest + "  ./" + path + '\n';
    if (quint64(line.size()) > maximumManifestBytes - quint64(replacementLines_.size())) return EFBIG;
    replaced_.insert(key(source)); replacementLines_.append(line);
    const QByteArray raw = QByteArray::fromHex(digest);
    return copied(path.constData(), quint64(info.st_size), reinterpret_cast<const unsigned char*>(raw.constData()));
}

int CheckMediaIntegritySource(const ISO_ANALYSIS_SOURCE& source, const ISO_ANALYSIS& analysis,
    bool runtime, unsigned splitParts, bool rockRidge, quint64& reserve, QString& problem)
{
    reserve = 0;
    const unsigned kind = analysis.report.has_md5sum;
    if (!runtime && kind == 0) return 0;
    if (kind > 2) return EINVAL;
    problem = QStringLiteral("The image's checksum list, validation backup names or integrity-file budget is unsupported.");
    IntegritySourcePlan plan;
    plan.runtime = runtime; plan.generate = runtime && kind != 1; plan.splitParts = splitParts;
    if (kind != 0) plan.manifestPath = manifestNames[kind];
    if (runtime) for (int arch = 1; arch < ARCH_MAX; ++arch) {
        QByteArray validator;
        const int resource = ReadMediaValidator(arch, validator);
        if (resource == ENOENT) continue;
        if (resource != 0) return resource;
        plan.validators.insert(QByteArray("efi/boot/boot") + efi_archname[arch] + ".efi", quint64(validator.size()));
    }
    ISO_TREE_WALK walk = {planIntegrityEntry, &plan, rockRidge, 0};
    if (source.iso != nullptr) walk.joliet_level = iso9660_ifs_get_joliet_level(source.iso);
    const int status = source.udf != nullptr ? WalkUdfFiles(udf_get_root(source.udf, true, 0), "", &walk)
        : source.iso != nullptr ? WalkIsoFiles(source.iso, "", &walk) : ISO_SCAN_ERROR;
    if (status != ISO_SCAN_OK) return plan.error != 0 ? plan.error : status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
    if (kind != 0) {
        ISO_FILE_READER reader;
        const QByteArray lookup = '/' + plan.manifestPath;
        const int opened = OpenIsoFileReader(&reader, source.iso, source.udf, lookup.constData());
        int error = opened == ISO_SCAN_OK ? 0 : opened == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
        if (error == 0 && reader.size != plan.manifestSize) error = ESTALE;
        QByteArray manifest;
        if (error == 0) {
            manifest.resize(qsizetype(reader.size));
            const int read = ReadIsoFileRange(&reader, 0, manifest.data(), size_t(manifest.size()));
            if (read != ISO_SCAN_OK) error = read == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
            if (manifest.contains('\0')) error = EBADMSG;
        }
        CloseIsoFileReader(&reader);
        if (error != 0) return error;
        quint64 expansion = 0; unsigned entries = 0; qsizetype position = 0;
        while (position < manifest.size()) {
            const qsizetype newline = manifest.indexOf('\n', position);
            const qsizetype end = newline < 0 ? manifest.size() : newline + 1;
            QByteArray path;
            if (checksumLine(manifest.mid(position, end - position), path)) {
                if (++entries > maximumManifestFiles) return EFBIG;
                if (runtime && plan.validators.contains(path.toLower())) expansion += 9;
            }
            position = end;
        }
        if (runtime && kind == 1 && entries == 0) return EBADMSG;
        // Generated SWM references can replace a source install.wim row.
        expansion += quint64(splitParts) * (maximumPathBytes + 35);
        if (expansion > maximumManifestBytes - plan.manifestSize) return EFBIG;
        reserve += 2 * (plan.manifestSize + expansion);
    }
    if (plan.generate) reserve += 2 * plan.generated;
    // Win7 EFI repair can supply a fallback absent from the input tree.
    if (runtime && plan.payloadBytes == 0) {
        QByteArray validator;
        const int error = ReadMediaValidator(ARCH_X86_64, validator);
        if (error != 0) return error;
        plan.payloadBytes = quint64(validator.size());
    }
    reserve += plan.payloadBytes;
    problem.clear();
    return 0;
}

int MediaIntegrity::updateManifest(QByteArray& bytes, const QSet<QString>& renamed, unsigned& updates)
{
    if (bytes.contains('\0')) return EBADMSG;
    QByteArray output; output.reserve(bytes.size());
    QHash<QString,QByteArray> hashes;
    qsizetype position = 0; unsigned entries = 0; bool replaced = false;
    while (position < bytes.size()) {
        if (IsRufusJobCancelled()) return ECANCELED;
        const qsizetype newline = bytes.indexOf('\n', position);
        const qsizetype end = newline < 0 ? bytes.size() : newline + 1;
        QByteArray line = bytes.mid(position, end - position); position = end;
        QByteArray path;
        if (checksumLine(line, path)) {
            if (++entries > maximumManifestFiles) return EFBIG;
            QString folded = key(path);
            for (auto move = relocations_.cbegin(); move != relocations_.cend(); ++move) {
                if (!folded.startsWith(move.key() + '/')) continue;
                const QByteArray suffix = path.mid(move.key().toUtf8().size());
                path = move.value() + suffix;
                line = line.left(34) + "./" + path + '\n';
                folded = key(path); ++updates; break;
            }
            if (replaced_.contains(folded)) {
                if (!replaced) {
                    if (quint64(replacementLines_.size()) > maximumManifestBytes - quint64(output.size())) return EFBIG;
                    output.append(replacementLines_); replaced = true;
                }
                ++updates;
                continue;
            }
            if (modified_.contains(folded)) {
                if (!hashes.contains(folded)) {
                    QByteArray digest;
                    const int error = hashRegular(root_, aliases_.value(folded,path), digest);
                    if (error != 0) return error;
                    hashes.insert(folded, digest);
                }
                line.replace(0, 32, hashes.value(folded)); ++updates;
            }
            if (renamed.contains(folded)) {
                const qsizetype extension = line.toLower().lastIndexOf(".efi");
                if (extension < 34) return EBADMSG;
                line.insert(extension, "_original");
            }
        }
        if (quint64(line.size()) > maximumManifestBytes - quint64(output.size())) return EFBIG;
        output.append(line);
    }
    bytes = std::move(output);
    return 0;
}

int MediaIntegrity::publish(const QString& name, const QByteArray& bytes)
{
    if (IsRufusJobCancelled()) return ECANCELED;
    QTemporaryFile staged(QStringLiteral("/proc/self/fd/%1/.rufus-md5-XXXXXX").arg(root_));
    if (!staged.open() || staged.write(bytes) != bytes.size() || !staged.flush()) return EIO;
    int error = syncFile(staged.handle(), syncError_);
    if (error == 0) error = verifyBytes(staged.handle(), bytes);
    if (error != 0) return error;
    const QByteArray temporary = QFile::encodeName(QFileInfo(staged.fileName()).fileName());
    QByteArray destination = QFile::encodeName(name), actual;
    const int existing = childName(root_, destination, actual);
    if (existing != 0 && existing != ENOENT) return existing;
    if (existing == 0) {
        struct stat info = {};
        if (fstatat(root_, actual.constData(), &info, AT_SYMLINK_NOFOLLOW) != 0) return errno;
        if (!S_ISREG(info.st_mode) || info.st_nlink != 1) return ENOTSUP;
        destination = actual;
    }
    if (renameat(root_, temporary.constData(), root_, destination.constData()) != 0) return errno;
    staged.setAutoRemove(false);
    return syncFile(root_, syncError_);
}

int MediaIntegrity::installValidators(QSet<QString>& renamed, MediaIntegrityResult& result)
{
    int boot = -1;
    int error = openRelative(root_, "efi/boot", boot);
    if (error != 0) return error;
    struct Validator { QByteArray name, backup, bytes; };
    QList<Validator> validators;
    for (int architecture = 1; architecture < ARCH_MAX && error == 0; ++architecture) {
        const QByteArray expected = QByteArray("boot") + efi_archname[architecture] + ".efi";
        QByteArray actual;
        error = childName(boot, expected, actual);
        if (error == ENOENT) { error = 0; continue; }
        if (error != 0) break;
        struct stat file = {};
        if (fstatat(boot, actual.constData(), &file, AT_SYMLINK_NOFOLLOW) != 0) { error = errno; break; }
        if (!S_ISREG(file.st_mode) || file.st_nlink != 1) { error = ENOTSUP; break; }
        Validator selected; selected.name = actual;
        selected.backup = actual.left(actual.size() - 4) + "_original" + actual.right(4);
        error = ReadMediaValidator(architecture, selected.bytes);
        // Windows has no bundled IA64/EBC validator. Preserve those entries.
        if (error == ENOENT) { error = 0; continue; }
        if (error != 0) break;
        QByteArray collision;
        const int backup = childName(boot, selected.backup, collision);
        if (backup != ENOENT) { error = backup == 0 ? EEXIST : backup; break; }
        validators.append(selected);
    }
    if (error == 0 && validators.isEmpty()) error = ENOENT;
    for (const auto& selected : validators) {
        if (error != 0) break;
        if (IsRufusJobCancelled()) { error = ECANCELED; break; }
        if (syscall(SYS_renameat2, boot, selected.name.constData(), boot, selected.backup.constData(), RENAME_NOREPLACE) != 0) {
            error = errno;
            const bool unsupported = error == ENOSYS || error == EINVAL || error == EOPNOTSUPP;
            if (!unsupported) break;
            // NTFS-3G uses the same no-replace hard-link fallback as the
            // existing native Syslinux config backup. Preserve either name
            // on failure and keep the complete creation job FAILED.
            if (linkat(boot, selected.name.constData(), boot, selected.backup.constData(), 0) != 0
                || unlinkat(boot, selected.name.constData(), 0) != 0) { error = errno; break; }
            error = 0;
        }
        const int file = openat(boot, selected.name.constData(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (file < 0) { error = errno; break; }
        qsizetype done = 0;
        while (error == 0 && done < selected.bytes.size()) {
            const ssize_t count = write(file, selected.bytes.constData() + done, size_t(selected.bytes.size() - done));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) error = count < 0 ? errno : EIO;
            else done += count;
        }
        if (error == 0) error = syncFile(file, syncError_);
        if (error == 0) error = verifyBytes(file, selected.bytes);
        if (::close(file) != 0 && error == 0) error = errno;
        if (error == 0) {
            renamed.insert(key(QByteArray("efi/boot/") + selected.name));
            ++result.validators;
        }
    }
    if (error == 0) error = syncFile(boot, syncError_);
    if (::close(boot) != 0 && error == 0) error = errno;
    return error;
}

int MediaIntegrity::finish(MediaIntegrityResult& result)
{
    result = {}; result.files = files_; result.bytes = bytes_;
    if (!active()) return EINVAL;
    int error = IsRufusJobCancelled() ? ECANCELED : 0;
    QByteArray existing, generated;
    if (error == 0 && kind_ != 0) error = readRegular(root_, manifestNames[kind_], existing);
    if (error == 0 && generated_) {
        if (!generated_->flush() || !generated_->seek(0)) error = EIO;
        else generated = generated_->readAll();
        if (error == 0 && quint64(generated.size()) != generatedBytes_) error = EIO;
    }
    // Hash changed files before moving any EFI entry. The original loader's
    // checksum is subsequently associated with its preserved _original name.
    const QSet<QString> noRenames;
    if (error == 0 && kind_ != 0) error = updateManifest(existing, noRenames, result.updated);
    if (error == 0 && generated_) error = updateManifest(generated, noRenames, result.updated);
    QSet<QString> renamed;
    if (error == 0 && runtime_) error = installValidators(renamed, result);
    modified_.clear(); relocations_.clear(); aliases_.clear(); replaced_.clear();
    if (error == 0 && runtime_ && kind_ != 0) error = updateManifest(existing, renamed, result.updated);
    if (error == 0 && runtime_ && generated_) error = updateManifest(generated, renamed, result.updated);
    if (error == 0 && generated_) generated.prepend("# md5sum_totalbytes = 0x" + QByteArray::number(bytes_, 16) + '\n');
    if (error == 0 && kind_ != 0) error = publish(QString::fromLatin1(manifestNames[kind_]), existing);
    if (error == 0 && generated_) error = publish(QStringLiteral("md5sum.txt"), generated);
    if (error == 0) error = syncFile(root_, syncError_);
    result.syncError = syncError_;
    const int closed = close();
    return error != 0 ? error : closed;
}

int MediaIntegrity::close()
{
    generated_.reset();
    int error = 0;
    if (root_ >= 0 && ::close(root_) != 0) error = errno;
    root_ = -1; modified_.clear(); relocations_.clear(); aliases_.clear(); replaced_.clear(); replacementLines_.clear();
    return error;
}
}
