#include "efi_architecture.h"
// SPDX-License-Identifier: GPL-3.0-or-later
// Native bounded adapters for src/hash.c's Secure Boot classification.
#include "secure_boot.h"
#include "embedded_efi_files.h"
#include "rufus_reporting.h"
#include "hash_native.h"
#include "iso_file.h"
#include "pe_arch.h"
#include "secure_boot_db.h"
#include <QFile>
#include <QtEndian>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <openssl/err.h>
#include <openssl/pkcs7.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <openssl/x509v3.h>

static void initializeSecureBootResources()
{
    Q_INIT_RESOURCE(secure_boot);
}
namespace rufus
{
namespace
{
constexpr quint32 sectionHeaderBytes = 40, maximumSections = 4096;
constexpr qsizetype minimumPe256Bytes = 1024, maximumPolicyBytes = 16 * 1024 * 1024;
struct PeSection { QByteArray name; quint32 address, offset, size; };
struct PeImage
{
    const QByteArray& bytes;
    quint32 optional = 0, directories = 0, directoryCount = 0, headers = 0, alignment = 0;
    QList<PeSection> sections;
    explicit PeImage(const QByteArray& input) : bytes(input) {}
    bool range(quint64 offset, quint64 size) const
    {
        return offset <= quint64(bytes.size()) && size <= quint64(bytes.size()) - offset;
    }
    quint16 word(quint32 offset) const { return qFromLittleEndian<quint16>(bytes.constBegin() + offset); }
    quint32 dword(quint32 offset) const { return qFromLittleEndian<quint32>(bytes.constBegin() + offset); }
    bool parse()
    {
        if (bytes.size() < 64 || bytes.size() > MaximumBootloaderBytes || bytes.left(2) != "MZ") return false;
        const quint32 pe = dword(60);
        if (pe < 64 || pe > INT32_MAX || !range(pe, 24) || bytes.mid(pe, 4) != QByteArray("PE\0\0", 4)) return false;
        const quint32 count = word(pe + 6), optionalSize = word(pe + 20);
        optional = pe + 24;
        if (!range(optional, optionalSize) || optionalSize < 2 || count > maximumSections) return false;
        const quint16 magic = word(optional);
        const quint32 directoryOffset = magic == 0x10b ? 96 : magic == 0x20b ? 112 : 0;
        if (directoryOffset == 0 || optionalSize < directoryOffset) return false;
        directoryCount = dword(optional + directoryOffset - 4);
        if (directoryCount > (optionalSize - directoryOffset) / 8) return false;
        directories = optional + directoryOffset;
        headers = dword(optional + 60); alignment = dword(optional + 36);
        const bool validAlignment = alignment != 0 && (alignment & (alignment - 1)) == 0;
        const quint32 table = optional + optionalSize;
        if (!validAlignment || !range(table, quint64(count) * sectionHeaderBytes)
            || headers < quint64(table) + quint64(count) * sectionHeaderBytes || !range(0, headers)) return false;
        for (quint32 index = 0; index < count; ++index) {
            const quint32 offset = table + index * sectionHeaderBytes;
            PeSection section{bytes.mid(offset, 8), dword(offset + 12), dword(offset + 20), dword(offset + 16)};
            if (!range(section.offset, section.size)) return false;
            sections.append(section);
        }
        return true;
    }
    bool directory(quint32 index, quint32& address, quint32& size) const
    {
        address = size = 0;
        if (index >= directoryCount) return false;
        address = dword(directories + index * 8); size = dword(directories + index * 8 + 4);
        return address != 0 && size != 0;
    }
    bool physical(quint32 rva, quint32 size, quint32& offset) const
    {
        for (const auto& section : sections) {
            if (rva < section.address) continue;
            const quint32 delta = rva - section.address;
            if (delta > section.size || size > section.size - delta) continue;
            const quint64 position = quint64(section.offset) + delta;
            if (!range(position, size)) return false;
            offset = quint32(position); return true;
        }
        return false;
    }
};
struct Span { quint32 offset, size; };
bool beforeSpan(const Span& first, const Span& second) { return first.offset < second.offset; }
int peDigest(const PeImage& pe, QByteArray& digest)
{
    // src/hash.c:efi_image_parse hashes ordered disjoint header/section/extra
    // regions, excluding CheckSum, the security directory and certificate tail.
    if (pe.bytes.size() < minimumPe256Bytes) return ENOTSUP;
    const quint32 checksum = pe.optional + 64;
    QList<Span> spans{{0, checksum}};
    if (pe.directoryCount > 4) {
        const quint32 security = pe.directories + 32;
        if (security < checksum + 4 || pe.headers < security + 8) return EBADMSG;
        spans.append({checksum + 4, security - checksum - 4});
        spans.append({security + 8, pe.headers - security - 8});
    } else {
        if (pe.headers < checksum + 4) return EBADMSG;
        spans.append({checksum + 4, pe.headers - checksum - 4});
    }
    quint64 hashed = pe.headers;
    for (const auto& section : pe.sections) {
        if (section.size == 0) continue;
        const quint64 size = (quint64(section.size) + pe.alignment - 1) & ~(quint64(pe.alignment) - 1);
        if (size > UINT32_MAX || !pe.range(section.offset, size)) return EBADMSG;
        spans.append({section.offset, quint32(size)}); hashed += size;
    }
    quint32 certificate = 0, certificateSize = 0;
    pe.directory(4, certificate, certificateSize);
    if (certificateSize != 0 && !pe.range(certificate, certificateSize)) return EBADMSG;
    if (hashed + certificateSize < quint64(pe.bytes.size()))
        spans.append({quint32(hashed), quint32(quint64(pe.bytes.size()) - certificateSize - hashed)});
    std::sort(spans.begin(), spans.end(), beforeSpan);
    HASH_CONTEXT context = {}; hash_init[HASH_SHA256](&context);
    quint64 end = 0;
    for (const auto& span : spans) {
        if (span.offset < end || !pe.range(span.offset, span.size)) return EBADMSG;
        if (IsIsoScanCancelled()) return ECANCELED;
        hash_write[HASH_SHA256](&context, reinterpret_cast<const uint8_t*>(pe.bytes.constData() + span.offset), span.size);
        end = quint64(span.offset) + span.size;
    }
    hash_final[HASH_SHA256](&context);
    digest = QByteArray(reinterpret_cast<const char*>(context.buf), SHA256_HASHSIZE);
    return 0;
}

// Prefer a unique embedded immediate issuer, then a unique cached issuer.
// Verify the child/issuer relationship; chain trust and validity dates do not
// authenticate a publisher here. Ambiguous/missing issuers retain the signer.
X509* uniqueIssuer(X509* signer, STACK_OF(X509)* candidates, bool& ambiguous)
{
    ambiguous = false;
    X509* selected = nullptr;
    for (int i = 0; i < sk_X509_num(candidates); ++i) {
        X509* candidate = sk_X509_value(candidates, i);
        if (X509_cmp(candidate, signer) == 0 || X509_check_issued(candidate, signer) != X509_V_OK) continue;
        EVP_PKEY* key = X509_get_pubkey(candidate);
        const bool matches = key && X509_verify(signer, key) == 1;
        EVP_PKEY_free(key);
        if (!matches) continue;
        if (selected && X509_cmp(selected, candidate) != 0) { ambiguous = true; return nullptr; }
        selected = candidate;
    }
    return selected;
}

void loadSystemIssuers(SecureBootPolicy& policy)
{
    constexpr qsizetype maximumBundleBytes = 16 * 1024 * 1024;
    // The native OpenSSL configured bundle is cached local material. Never use
    // AIA, a network lookup, or caller-controlled SSL_CERT_FILE overrides.
    QFile file(QString::fromLocal8Bit(X509_get_default_cert_file()));
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > maximumBundleBytes) return;
    const QByteArray bytes = file.read(maximumBundleBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > maximumBundleBytes) return;
    BIO* input = BIO_new_mem_buf(bytes.constData(), int(bytes.size()));
    if (!input) return;
    for (int i = 0; i < 4096; ++i) {
        X509* cert = PEM_read_bio_X509(input, nullptr, nullptr, nullptr);
        if (!cert) break;
        const int size = i2d_X509(cert, nullptr);
        if (size > 0 && size <= 65536) {
            QByteArray der(size, '\0'); auto* output = reinterpret_cast<unsigned char*>(der.data());
            if (i2d_X509(cert, &output) == size) policy.cachedIssuers.append(der);
        }
        X509_free(cert);
    }
    BIO_free(input); ERR_clear_error();
}

int certificateInfo(const PeImage& pe, const SecureBootPolicy& policy, BootloaderSecurity& result)
{
    quint32 offset = 0, size = 0;
    if (!pe.directory(4, offset, size)) return 0;
    if (!pe.range(offset, size) || size < 8) return EBADMSG;
    const quint32 length = pe.dword(offset);
    if (length == 0) return 0;
    const bool pkcsCertificate = pe.word(offset + 4) == 0x200 && pe.word(offset + 6) == 2;
    if (!pkcsCertificate || length < 8 || length > size || length > quint32(maximumPolicyBytes)) return EBADMSG;
    const auto* start = reinterpret_cast<const unsigned char*>(pe.bytes.constData() + offset + 8);
    const unsigned char* position = start;
    PKCS7* message = d2i_PKCS7(nullptr, &position, length - 8);
    int error = EBADMSG;
    STACK_OF(X509)* chain = nullptr;
    if (message == nullptr || !PKCS7_type_is_signed(message) || message->d.sign == nullptr) goto out;
    // Certificate length may include at most alignment padding after DER.
    if (position < start || position > start + length - 8 || start + length - 8 - position > 7) goto out;
    while (position < start + length - 8) if (*position++ != 0) goto out;
    {
        STACK_OF(PKCS7_SIGNER_INFO)* signers = PKCS7_get_signer_info(message);
        if (sk_PKCS7_SIGNER_INFO_num(signers) <= 0) goto out;
        X509* signer = PKCS7_cert_from_signer_info(message, sk_PKCS7_SIGNER_INFO_value(signers, 0));
        if (signer == nullptr) goto out;
        bool ambiguous = false;
        X509* selected = uniqueIssuer(signer, message->d.sign->cert,ambiguous);
        chain = sk_X509_new_null();
        if (!chain) { error = ENOMEM; goto out; }
        if (!selected && !ambiguous && policy.cachedIssuers.size() <= 4096) {
            qsizetype total = 0;
            for (const QByteArray& der : policy.cachedIssuers) {
                total += der.size();
                if (der.isEmpty() || der.size() > 65536 || total > 16 * 1024 * 1024) break;
                const auto* input = reinterpret_cast<const unsigned char*>(der.constData());
                const auto* end = input + der.size();
                X509* cert = d2i_X509(nullptr, &input, der.size());
                if (!cert || input != end) { X509_free(cert); continue; }
                if (!sk_X509_push(chain, cert)) { X509_free(cert); error = ENOMEM; goto out; }
            }
            selected = uniqueIssuer(signer, chain,ambiguous);
        }
        const bool hasIssuer = selected != nullptr;
        if (!selected) selected = signer;
        unsigned hashSize = 0; unsigned char thumbprint[EVP_MAX_MD_SIZE];
        if (X509_digest(selected, EVP_sha1(), thumbprint, &hashSize) != 1 || hashSize != 20) goto out;
        char name[256] = {};
        if (X509_NAME_get_text_by_NID(X509_get_subject_name(selected), NID_commonName, name, sizeof(name)) <= 0) goto out;
        result.signedImage = true; result.issuer = hasIssuer;
        result.certificateName = QString::fromUtf8(name);
        result.certificateSha1 = QByteArray(reinterpret_cast<const char*>(thumbprint), 20);
        error = 0;
    }
out:
    sk_X509_pop_free(chain, X509_free); PKCS7_free(message);
    ERR_clear_error();
    return error;
}

int thumbprints(const QByteArray& text, const QByteArray& selected, bool& found)
{
    found = false;
    if (text.size() > maximumPolicyBytes || text.contains('\0')) return EBADMSG;
    for (const auto& row : text.split('\n')) {
        const QByteArray value = row.trimmed();
        if (value.isEmpty() || value.startsWith('#')) continue;
        if (value.size() != 40) return EBADMSG;
        for (char digit : value) {
            const bool hex = (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') || (digit >= 'A' && digit <= 'F');
            if (!hex) return EBADMSG;
        }
        if (QByteArray::fromHex(value) == selected) found = true;
    }
    return 0;
}
int dbxContains(const QByteArray& dbx, const QByteArray& digest, bool& found)
{
    found = false;
    if (dbx.isEmpty()) return 0; // Original resources for these architectures are empty.
    constexpr quint32 efiTimeBytes = 16, winCertificateHeader = 24, signatureListHeader = 28, ownerBytes = 16;
    if (dbx.size() < efiTimeBytes + winCertificateHeader || dbx.size() > maximumPolicyBytes) return EBADMSG;
    const quint32 authLength = qFromLittleEndian<quint32>(dbx.constData() + efiTimeBytes);
    if (authLength < winCertificateHeader || authLength > quint64(dbx.size()) - efiTimeBytes) return EBADMSG;
    quint64 position = efiTimeBytes + authLength;
    if (position >= quint64(dbx.size())) return EBADMSG;
    const QByteArray sha256Guid = QByteArray::fromHex("2616c4c14c509240aca941f936934328");
    while (position < quint64(dbx.size())) {
        const quint64 remaining = quint64(dbx.size()) - position;
        if (remaining < signatureListHeader) return EBADMSG;
        const char* list = dbx.constData() + position;
        const quint32 size = qFromLittleEndian<quint32>(list + 16), header = qFromLittleEndian<quint32>(list + 20), entry = qFromLittleEndian<quint32>(list + 24);
        if (size < signatureListHeader || size > remaining || header > size - signatureListHeader || entry < ownerBytes) return EBADMSG;
        const quint32 payload = size - signatureListHeader - header;
        if (payload % entry != 0) return EBADMSG;
        const bool sha256 = QByteArray(list, 16) == sha256Guid;
        if (sha256 && entry != ownerBytes + SHA256_HASHSIZE) return EBADMSG;
        if (sha256) for (quint32 index = 0; index < payload / entry; ++index) {
            if (memcmp(list + signatureListHeader + header + quint64(index) * entry + ownerBytes, digest.constData(), SHA256_HASHSIZE) == 0) found = true;
        }
        position += size;
    }
    return 0;
}
int sbatVersions(const QByteArray& text, QHash<QByteArray,quint32>& versions, bool thresholds)
{
    if (text.size() > maximumPolicyBytes) return EBADMSG;
    for (const auto& row : text.split('\n')) {
        const QByteArray line = row.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const auto fields = line.split(',');
        if (fields.size() < 2 || fields[0].isEmpty()) return EBADMSG;
        bool okay = false;
        const QByteArray number = fields[1];
        const quint32 version = number.toUInt(&okay, number.startsWith("0x") ? 16 : 10);
        if (!okay) return EBADMSG;
        if (version != 0) {
            const auto existing = versions.constFind(fields[0]);
            const quint32 selected = existing == versions.cend() ? version
                : thresholds ? qMax(version, existing.value()) : qMin(version, existing.value());
            versions.insert(fields[0], selected);
        }
    }
    return 0;
}

int resourceSvn(const PeImage& pe, quint32 base, quint32 size, quint32 directory, const QByteArray& wanted,
    bool selected, unsigned depth, unsigned& visited, quint32& version, bool& found)
{
    constexpr quint32 resourceDirectoryBytes = 16, entryBytes = 8, maximumResourceEntries = 65536;
    if (depth > 16 || ++visited > maximumResourceEntries || directory > size || size - directory < resourceDirectoryBytes) return EBADMSG;
    const quint32 offset = base + directory;
    const quint32 named = pe.word(offset + 12), count = named + pe.word(offset + 14);
    if (count > (size - directory - resourceDirectoryBytes) / entryBytes || count > maximumResourceEntries - visited) return EBADMSG;
    visited += count;
    for (quint32 index = 0; index < count; ++index) {
        if (IsIsoScanCancelled()) return ECANCELED;
        const quint32 entry = offset + resourceDirectoryBytes + index * entryBytes;
        const quint32 name = pe.dword(entry), target = pe.dword(entry + 4);
        bool matched = selected;
        if (!matched && (name & 0x80000000)) {
            const quint32 string = name & 0x7fffffff;
            if (string > size || size - string < 2) return EBADMSG;
            const quint32 length = pe.word(base + string);
            if (length > (size - string - 2) / 2) return EBADMSG;
            if (length != quint32(wanted.size())) continue;
            matched = true;
            for (quint32 letter = 0; letter < length; ++letter)
                if (pe.word(base + string + 2 + letter * 2) != quint8(wanted[letter])) matched = false;
            if (!matched) continue;
        }
        const quint32 next = target & 0x7fffffff;
        if (target & 0x80000000) {
            const int error = resourceSvn(pe, base, size, next, wanted, matched, depth + 1, visited, version, found);
            if (error != 0 || found) return error;
        } else if (matched) {
            if (next > size || size - next < 16) return EBADMSG;
            const quint32 rva = pe.dword(base + next), length = pe.dword(base + next + 4);
            quint32 data = 0;
            if (length != 4 || !pe.physical(rva, length, data)) return EBADMSG;
            version = pe.dword(data); found = true; return 0;
        }
    }
    return 0;
}

int revocation(const PeImage& pe, const SecureBootPolicy& policy, bool expert, BootloaderSecurity& result)
{
    int error = peDigest(pe, result.pe256);
    if (error != 0) return error;
    bool found = false;
    error = dbxContains(policy.dbx.value(NativeEfiArchitecture(pe.bytes.constData(), size_t(pe.bytes.size()))), result.pe256, found);
    if (error != 0) return error;
    if (found) { result.revocation = 1; return 0; }
    if (policy.ssp.size() > maximumPolicyBytes || policy.ssp.size() % SHA256_HASHSIZE != 0) return EBADMSG;
    for (qsizetype offset = 0; offset < policy.ssp.size(); offset += SHA256_HASHSIZE)
        if (policy.ssp.mid(offset, SHA256_HASHSIZE) == result.pe256) { result.revocation = 2; return 0; }
    QHash<QByteArray,quint32> minimum;
    error = sbatVersions(policy.sbat, minimum, true);
    if (error != 0) return error;
    for (const auto& section : pe.sections) {
        if (section.name != QByteArray(".sbat\0\0\0", 8)) continue;
        QByteArray text = pe.bytes.mid(section.offset, section.size);
        const qsizetype nul = text.indexOf('\0'); if (nul >= 0) text.truncate(nul);
        QHash<QByteArray,quint32> actual;
        error = sbatVersions(text, actual, false);
        if (error != 0) return error;
        for (auto it = actual.cbegin(); it != actual.cend(); ++it)
            if (minimum.contains(it.key()) && it.value() < minimum.value(it.key())) { result.revocation = 3; return 0; }
    }
    quint32 resource = 0, resourceSize = 0, base = 0;
    if (pe.directory(2, resource, resourceSize)) {
        if (!pe.physical(resource, resourceSize, base)) return EBADMSG;
        for (auto it = minimum.cbegin(); it != minimum.cend(); ++it) {
            const QByteArray name = it.key();
            if (name != name.toUpper()) continue;
            quint32 version = 0; unsigned visited = 0; found = false;
            error = resourceSvn(pe, base, resourceSize, 0, name, false, 0, visited, version, found);
            if (error != 0) return error;
            if (found && version < it.value()) { result.revocation = 4; return 0; }
        }
    }
    if (expert) {
        error = thumbprints(policy.revokedCertificates, result.certificateSha1, found);
        if (error != 0) return error;
        if (found) result.revocation = 5;
    }
    return 0;
}
} // namespace

int ValidateSecureBootList(int kind, const QByteArray& bytes)
{
    if (kind < 0 || kind > 2 || bytes.isEmpty() || bytes.size() >= 1024 || bytes.contains('\0')) return EBADMSG;
    if (kind == 0) {
        QHash<QByteArray,quint32> entries;
        const int error = sbatVersions(bytes, entries, true);
        return error != 0 ? error : entries.isEmpty() ? EBADMSG : 0;
    }
    bool found = false;
    const int error = thumbprints(bytes, {}, found);
    if (error != 0) return error;
    for (const auto& row : bytes.split('\n')) {
        const auto entry = row.trimmed();
        if (!entry.isEmpty() && !entry.startsWith('#')) return 0;
    }
    return EBADMSG;
}
int ValidateSecureBootDbx(const QByteArray& bytes)
{
    if (bytes.isEmpty()) return EBADMSG;
    bool found = false;
    return dbxContains(bytes, QByteArray(SHA256_HASHSIZE, '\0'), found);
}

int LoadBundledSecureBootPolicy(SecureBootPolicy& policy)
{
    static const bool initialized = (initializeSecureBootResources(), true);
    Q_UNUSED(initialized);
    SecureBootPolicy loaded;
    loaded.sbat = db_sbat_level_txt; loaded.activeCertificates = db_sb_active_txt; loaded.revokedCertificates = db_sb_revoked_txt;
    const char* names[] = {"ia32", "x64", "arm", "aa64", "ia64", "riscv64", "loongarch64"};
    for (int architecture = ARCH_X86_32; architecture <= ARCH_LOONGARCH_64; ++architecture) {
        QFile file(QStringLiteral(":/rufus/dbx/") + QString::fromLatin1(names[architecture - 1]));
        if (!file.open(QIODevice::ReadOnly)) return EIO;
        if (file.size() > maximumPolicyBytes) return EFBIG;
        loaded.dbx.insert(architecture, file.readAll());
    }
    loadSystemIssuers(loaded);
    policy = std::move(loaded); return 0;
}
int BootloaderPe256(const QByteArray& bytes, QByteArray& digest)
{
    digest.clear(); PeImage pe(bytes);
    if (!pe.parse()) return EBADMSG;
    return peDigest(pe, digest);
}
int InspectBootloaderSecurity(const QByteArray& bytes, const SecureBootPolicy& policy, bool expert, BootloaderSecurity& result)
{
    result = {}; BootloaderSecurity checked;
    if (IsIsoScanCancelled()) return ECANCELED;
    PeImage pe(bytes); if (!pe.parse()) return EBADMSG;
    int error = certificateInfo(pe, policy, checked);
    if (error == 0 && checked.signedImage) {
        bool active = false;
        error = thumbprints(policy.activeCertificates, checked.certificateSha1, active);
        checked.secureBootAuthority = checked.issuer && active;
        if (error == 0) error = revocation(pe, policy, expert, checked);
    }
    if (error == 0) result = std::move(checked);
    return error;
}
namespace {
struct EmbeddedSecurity { const ISO_ANALYSIS_SOURCE* source; ISO_ANALYSIS* analysis; const SecureBootPolicy* policy; bool expert; bool any = false; };
int inspectEmbeddedSecurity(const EMBEDDED_EFI_FILE* file, void* opaque)
{
    auto& scan = *static_cast<EmbeddedSecurity*>(opaque);
    const QByteArray path(file->path);
    if (file->directory || !path.toLower().endsWith(".efi")) return 0;
    bool copy = false;
    int error = EmbeddedEfiPrecedence(scan.source->iso,scan.source->udf,&scan.analysis->report,file,&copy);
    if (error || !copy) return error;
    if (!file->size || file->size > quint64(MaximumBootloaderBytes)) return EFBIG;
    QByteArray bytes(qsizetype(file->size),'\0');
    error = ReadEmbeddedEfiRange(file,0,bytes.data(),size_t(bytes.size()));
    BootloaderSecurity info;
    if (!error) error = InspectBootloaderSecurity(bytes,*scan.policy,scan.expert,info);
    if (error) return error;
    scan.any = true;
    uprintf("  %s (embedded EFI image)%s",file->path,info.secureBootAuthority ? "*" : "");
    if (info.signedImage) uprintf("  Signed by '%s'",qPrintable(info.certificateName));
    if (info.secureBootAuthority) scan.analysis->report.has_secureboot_bootloader |= 1;
    if (info.revocation) {
        scan.analysis->report.has_secureboot_bootloader |= quint8(1 << info.revocation);
        uprintf("  WARNING: Embedded '%s' has been revoked (policy %d)",file->path,info.revocation);
    }
    return 0;
}
}
int ScanIsoSecureBoot(const ISO_ANALYSIS_SOURCE& source, ISO_ANALYSIS& analysis, const SecureBootPolicy& policy, bool expert)
{
    if (!(analysis.pending & ISO_PENDING_SECURE_BOOT)) return analysis.pending ? ISO_SCAN_PARTIAL : ISO_SCAN_OK;
    bool complete = true, any = false;
    analysis.report.has_secureboot_bootloader = 0;
    uprintf("UEFI bootloaders analysis:");
    const char* reasons[] = {"UEFI DBX", "Windows SSP", "Linux SBAT", "Windows SVN", "Cert DBX"};
    for (const auto& entry : analysis.report.efi_boot_entry) {
        if (entry.path[0] == 0) continue;
        if ((analysis.report.has_efi & 0x4000) && strcasecmp(entry.path,"/efi/boot/bootx64.efi") == 0) continue;
        any = true;
        if (IsIsoScanCancelled()) return ISO_SCAN_CANCELLED;
        ISO_FILE_READER file = {};
        int status = OpenIsoFileReader(&file, source.iso, source.udf, entry.path);
        int error = status == ISO_SCAN_CANCELLED ? ECANCELED : status == ISO_SCAN_OK ? 0 : EIO;
        if (error == 0 && (file.size == 0 || file.size > quint64(MaximumBootloaderBytes))) error = EFBIG;
        QByteArray bytes;
        if (error == 0) {
            bytes.resize(qsizetype(file.size));
            status = ReadIsoFileRange(&file, 0, bytes.data(), size_t(bytes.size()));
            if (status != ISO_SCAN_OK) error = status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
        }
        CloseIsoFileReader(&file);
        BootloaderSecurity info;
        if (error == 0) error = InspectBootloaderSecurity(bytes, policy, expert, info);
        if (error == ECANCELED) return ISO_SCAN_CANCELLED;
        if (error != 0) { complete = false; uprintf("  Warning: Could not inspect '%s': %s", entry.path, strerror(error)); continue; }
        uprintf("  %s%s", entry.path, info.secureBootAuthority ? "*" : "");
        if (info.secureBootAuthority) analysis.report.has_secureboot_bootloader |= 1;
        if (info.signedImage) uprintf("  Signed by '%s'", qPrintable(info.certificateName));
        else uprintf("  (Unsigned Bootloader)");
        if (info.revocation != 0) {
            analysis.report.has_secureboot_bootloader |= quint8(1 << info.revocation);
            uprintf("  WARNING: '%s' has been revoked by %s", entry.path, reasons[info.revocation - 1]);
        }
        else if (info.certificateName == QStringLiteral("Microsoft Windows Production PCA 2011")) {
            uprintf("  Note: This bootloader may fail Secure Boot validation on systems updated to use the 'Windows UEFI CA 2023' certificate.");
        } else if (info.certificateName == QStringLiteral("Windows UEFI CA 2023")) {
            uprintf("  Note: This bootloader will fail Secure Boot validation on systems without the latest Secure Boot certificates.");
        } else {
            const bool thirdParty = info.certificateName == QStringLiteral("Microsoft Corporation UEFI CA 2011")
                || info.certificateName == QStringLiteral("Microsoft UEFI CA 2023");
            if (thirdParty) uprintf("  Note: Some systems require enabling 'Microsoft 3rd-party UEFI CA' in firmware for this bootloader.");
        }
    }
    ISO_FILE_READER embedded = {};
    const int embeddedStatus = (analysis.report.has_efi & 0xc000) && HAS_EFI_IMG(analysis.report)
        ? OpenIsoFileReader(&embedded,source.iso,source.udf,analysis.report.efi_img_path) : ISO_SCAN_PARTIAL;
    CloseIsoFileReader(&embedded);
    if (embeddedStatus == ISO_SCAN_CANCELLED) return ISO_SCAN_CANCELLED;
    if (embeddedStatus == ISO_SCAN_OK) {
        EmbeddedSecurity scan{&source,&analysis,&policy,expert};
        const int error = WalkEmbeddedEfiFiles(source.iso,source.udf,analysis.report.efi_img_path,inspectEmbeddedSecurity,&scan);
        if (error == ECANCELED) return ISO_SCAN_CANCELLED;
        if (error) { complete = false; uprintf("  Warning: Could not inspect embedded EFI files: %s",strerror(error)); }
        any |= scan.any;
    }
    if (complete && any) analysis.pending &= ~ISO_PENDING_SECURE_BOOT;
    return analysis.pending ? ISO_SCAN_PARTIAL : ISO_SCAN_OK;
}
} // namespace rufus
