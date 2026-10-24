// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/secure_boot.h"
#include "pe_arch.h"
#include <QCryptographicHash>
#include <QFile>
#include <QTest>
#include <QtEndian>
#include <openssl/pem.h>
#include <openssl/pkcs7.h>
#include <openssl/x509v3.h>
#include <cerrno>

#include "secure_boot_fixture.h"
using namespace secureboot_fixture;
namespace { bool cancelled = false; }
extern "C" int __wrap_IsIsoScanCancelled() { return cancelled; }

class TestSecureBoot : public QObject
{
    Q_OBJECT
    EVP_PKEY *rootKey_ = nullptr, *key_ = nullptr;
    X509 *root_ = nullptr, *leaf_ = nullptr;
    QByteArray signature_;
    rufus::SecureBootPolicy policy_;
    QByteArray signedImage(QByteArray bytes) const
    {
        const int offset = bytes.size(), table = 88 + (qFromLittleEndian<quint16>(bytes.constData() + 88) == 0x20b ? 112 : 96) + 32;
        QByteArray cert(8, '\0'); cert.append(signature_); const quint32 length = quint32(cert.size());
        dword(cert, 0, length); word(cert, 4, 0x200); word(cert, 6, 2);
        while (cert.size() % 8) cert.append('\0');
        bytes.append(cert); dword(bytes, table, quint32(offset)); dword(bytes, table + 4, quint32(cert.size())); return bytes;
    }
private slots:
    void initTestCase()
    {
        rootKey_ = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", size_t(2048));
        key_ = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", size_t(2048));
        QVERIFY(rootKey_ && key_);
        root_ = certificate(rootKey_, "Synthetic UEFI CA", nullptr, nullptr); QVERIFY(root_);
        leaf_ = certificate(key_, "Synthetic EFI signer", root_, rootKey_); QVERIFY(leaf_);
        STACK_OF(X509)* certificates = sk_X509_new_null(); QVERIFY(certificates);
        QVERIFY(sk_X509_push(certificates, root_) > 0);
        BIO* data = BIO_new_mem_buf("metadata classification fixture", 31); QVERIFY(data);
        PKCS7* message = PKCS7_sign(leaf_, key_, certificates, data, PKCS7_BINARY); QVERIFY(message);
        unsigned char* encoded = nullptr; const int size = i2d_PKCS7(message, &encoded); QVERIFY(size > 0);
        signature_ = QByteArray(reinterpret_cast<char*>(encoded), size);
        OPENSSL_free(encoded); PKCS7_free(message); BIO_free(data); sk_X509_free(certificates);
        QCOMPARE(rufus::LoadBundledSecureBootPolicy(policy_), 0);
    }
    void cleanupTestCase()
    {
        X509_free(leaf_); X509_free(root_); EVP_PKEY_free(key_); EVP_PKEY_free(rootKey_);
    }
    void init() { cancelled = false; }
    void peDigest_data()
    {
        QTest::addColumn<bool>("x64"); QTest::newRow("PE32") << false; QTest::newRow("PE32+") << true;
    }
    void peDigest()
    {
        QFETCH(bool, x64); const QByteArray bytes = signedImage(peImage(x64));
        const int security = 88 + (x64 ? 112 : 96) + 32;
        const QByteArray expected = QCryptographicHash::hash(bytes.left(152) + bytes.mid(156, security - 156)
            + bytes.mid(security + 8, 2048 - security - 8), QCryptographicHash::Sha256);
        QByteArray actual; QCOMPARE(rufus::BootloaderPe256(bytes, actual), 0); QCOMPARE(actual, expected);
        QByteArray checksum = bytes; dword(checksum, 152, 0x11223344);
        QCOMPARE(rufus::BootloaderPe256(checksum, actual), 0); QCOMPARE(actual, expected);
        QByteArray payload = bytes; payload[600] ^= 1;
        QCOMPARE(rufus::BootloaderPe256(payload, actual), 0); QVERIFY(actual != expected);
        QByteArray cert = bytes; cert[2058] ^= 1;
        QCOMPARE(rufus::BootloaderPe256(cert, actual), 0); QCOMPARE(actual, expected);
    }
    void classification()
    {
        auto policy = policy_; policy.activeCertificates = sha1(root_).toHex();
        rufus::BootloaderSecurity info;
        QCOMPARE(rufus::InspectBootloaderSecurity(signedImage(peImage()), policy, false, info), 0);
        QVERIFY(info.signedImage && info.issuer && info.secureBootAuthority); QCOMPARE(info.certificateName, QStringLiteral("Synthetic UEFI CA"));
        QCOMPARE(info.certificateSha1, sha1(root_)); QCOMPARE(info.revocation, 0);
        policy.activeCertificates = QByteArray(40, '0');
        QCOMPARE(rufus::InspectBootloaderSecurity(signedImage(peImage()), policy, false, info), 0); QVERIFY(!info.secureBootAuthority);
        QCOMPARE(rufus::InspectBootloaderSecurity(peImage(), policy, false, info), 0); QVERIFY(!info.signedImage && !info.secureBootAuthority);
    }
    void cachedImmediateIssuers()
    {
        BIO* data = BIO_new_mem_buf("metadata",8); QVERIFY(data);
        PKCS7* message = PKCS7_sign(leaf_,key_,nullptr,data,PKCS7_BINARY); QVERIFY(message);
        unsigned char* encoded = nullptr; const int length = i2d_PKCS7(message,&encoded); QVERIFY(length > 0);
        const QByteArray original = signature_; signature_ = QByteArray(reinterpret_cast<char*>(encoded),length);
        OPENSSL_free(encoded); PKCS7_free(message); BIO_free(data);
        const int certSize = i2d_X509(root_,nullptr); QVERIFY(certSize > 0);
        QByteArray der(certSize,'\0'); auto* output = reinterpret_cast<unsigned char*>(der.data()); QCOMPARE(i2d_X509(root_,&output),certSize);
        auto policy = policy_; policy.cachedIssuers.clear(); policy.activeCertificates = sha1(root_).toHex();
        rufus::BootloaderSecurity info;
        QCOMPARE(rufus::InspectBootloaderSecurity(signedImage(peImage()),policy,false,info),0);
        QVERIFY(info.signedImage && !info.issuer); QCOMPARE(info.certificateSha1,sha1(leaf_));
        policy.cachedIssuers = {QByteArray("invalid"),der,der};
        QCOMPARE(rufus::InspectBootloaderSecurity(signedImage(peImage()),policy,false,info),0);
        QVERIFY(info.issuer && info.secureBootAuthority); QCOMPARE(info.certificateSha1,sha1(root_));
        signature_ = original;
        policy.cachedIssuers = {QByteArray("unavailable")};
        QCOMPARE(rufus::InspectBootloaderSecurity(signedImage(peImage()),policy,false,info),0);
        QVERIFY(info.issuer); QCOMPARE(info.certificateSha1,sha1(root_)); // Embedded material takes precedence.
    }

    void ambiguousEmbeddedIssuerKeepsSigner()
    {
        X509* other = X509_dup(root_); QVERIFY(other);
        QVERIFY(ASN1_INTEGER_set(X509_get_serialNumber(other),3));
        QVERIFY(X509_sign(other,rootKey_,EVP_sha256()) > 0);
        STACK_OF(X509)* certificates = sk_X509_new_null(); QVERIFY(certificates);
        QVERIFY(sk_X509_push(certificates,root_) > 0); QVERIFY(sk_X509_push(certificates,other) > 0);
        BIO* data = BIO_new_mem_buf("metadata",8); QVERIFY(data);
        PKCS7* message = PKCS7_sign(leaf_,key_,certificates,data,PKCS7_BINARY); QVERIFY(message);
        unsigned char* encoded = nullptr; const int length = i2d_PKCS7(message,&encoded); QVERIFY(length > 0);
        const QByteArray original = signature_; signature_ = QByteArray(reinterpret_cast<char*>(encoded),length);
        OPENSSL_free(encoded); PKCS7_free(message); BIO_free(data); sk_X509_free(certificates); X509_free(other);
        auto policy = policy_; const int bytes = i2d_X509(root_,nullptr); QByteArray der(bytes,'\0');
        auto* output = reinterpret_cast<unsigned char*>(der.data()); QCOMPARE(i2d_X509(root_,&output),bytes);
        policy.cachedIssuers = {der};
        rufus::BootloaderSecurity info;
        const int error = rufus::InspectBootloaderSecurity(signedImage(peImage()),policy,false,info);
        signature_ = original;
        QCOMPARE(error,0); QVERIFY(info.signedImage && !info.issuer); QCOMPARE(info.certificateSha1,sha1(leaf_));
    }

    void revocation_data()
    {
        QTest::addColumn<int>("kind"); QTest::addColumn<bool>("expert");
        for (int kind = 1; kind <= 5; ++kind) for (bool expert : {false,true})
            QTest::newRow(qPrintable(QStringLiteral("kind-%1-expert-%2").arg(kind).arg(expert))) << kind << expert;
    }
    void revocation()
    {
        QFETCH(int, kind); QFETCH(bool, expert);
        const QByteArray bytes = signedImage(kind == 4 ? svnImage(0x60000) : sbatImage("sbat,1,fixture\nshim,3,Vendor\n"));
        auto policy = policy_; policy.dbx.clear(); policy.sbat.clear(); policy.activeCertificates = sha1(root_).toHex();
        policy.revokedCertificates.clear(); policy.ssp.clear();
        QByteArray digest; QCOMPARE(rufus::BootloaderPe256(bytes, digest), 0);
        if (kind == 1) policy.dbx.insert(ARCH_X86_64, dbxList(digest));
        if (kind == 2) policy.ssp = digest;
        if (kind == 3) policy.sbat = "shim,4";
        if (kind == 4) policy.sbat = "BOOTMGRSECURITYVERSIONNUMBER,0x70000";
        if (kind == 5) policy.revokedCertificates = sha1(root_).toHex();
        rufus::BootloaderSecurity info;
        QCOMPARE(rufus::InspectBootloaderSecurity(bytes, policy, expert, info), 0);
        QCOMPARE(info.revocation, kind == 5 && !expert ? 0 : kind); QVERIFY(info.secureBootAuthority);
        // Original policy only applies revocation checks to signed images.
        QCOMPARE(rufus::InspectBootloaderSecurity(peImage(), policy, expert, info), 0); QCOMPARE(info.revocation, 0);
    }
    void orderAndBoundaries()
    {
        auto policy = policy_; policy.sbat = "shim,4"; policy.ssp.clear();
        rufus::BootloaderSecurity info;
        QCOMPARE(rufus::InspectBootloaderSecurity(signedImage(sbatImage("shim,4,Vendor\n")), policy, false, info), 0); QCOMPARE(info.revocation, 0);
        QCOMPARE(rufus::InspectBootloaderSecurity(signedImage(sbatImage("shim,0,Vendor\n")), policy, false, info), 0); QCOMPARE(info.revocation, 0);
        QCOMPARE(rufus::InspectBootloaderSecurity(signedImage(sbatImage("shim,3,Vendor\nshim,4,Vendor\n")), policy, false, info), 0);
        QCOMPARE(info.revocation, 3); // A later duplicate must not conceal an earlier revoked entry.
        const auto bytes = signedImage(sbatImage("shim,3,Vendor\n"));
        QByteArray hash; QCOMPARE(rufus::BootloaderPe256(bytes, hash), 0);
        policy.ssp = hash;
        QCOMPARE(rufus::InspectBootloaderSecurity(bytes, policy, true, info), 0); QCOMPARE(info.revocation, 2);
        policy.dbx.insert(ARCH_X86_64, dbxList(hash));
        QCOMPARE(rufus::InspectBootloaderSecurity(bytes, policy, true, info), 0); QCOMPARE(info.revocation, 1);
        policy.dbx.clear(); policy.ssp.clear(); policy.sbat = "BOOTMGRSECURITYVERSIONNUMBER,0x70000";
        QCOMPARE(rufus::InspectBootloaderSecurity(signedImage(svnImage(0x70000)), policy, false, info), 0); QCOMPARE(info.revocation, 0);
    }
    void malformed_data()
    {
        QTest::addColumn<int>("fault");
        for (int fault = 0; fault < 14; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void malformed()
    {
        QFETCH(int, fault); QByteArray bytes = signedImage(peImage()); auto policy = policy_;
        switch (fault) {
        case 0: bytes.truncate(63); break;
        case 1: dword(bytes, 60, UINT32_MAX); break;
        case 2: word(bytes, 70, UINT16_MAX); break;
        case 3: word(bytes, 84, 1); break;
        case 4: word(bytes, 88, 0); break;
        case 5: dword(bytes, 124, 0); break;
        case 6: dword(bytes, 148, UINT32_MAX); break;
        case 7: dword(bytes, 88 + 108, UINT32_MAX); break;
        case 8: dword(bytes, 88 + 240 + 20, UINT32_MAX); break;
        case 9: dword(bytes, 88 + 112 + 36, UINT32_MAX); break;
        case 10: dword(bytes, 2048, UINT32_MAX); break;
        case 11: word(bytes, 2054, 1); break;
        case 12: bytes[2056] = 0; break;
        case 13: policy.activeCertificates = "not a thumbprint"; break;
        }
        rufus::BootloaderSecurity info; info.signedImage = true;
        QVERIFY(rufus::InspectBootloaderSecurity(bytes, policy, true, info) != 0); QVERIFY(!info.signedImage && info.pe256.isEmpty());
    }
    void corruptDbxAndResource()
    {
        const QByteArray bytes = signedImage(peImage()); QByteArray hash;
        QCOMPARE(rufus::BootloaderPe256(bytes, hash), 0);
        for (int fault = 0; fault < 5; ++fault) {
            auto policy = policy_; QByteArray dbx = dbxList(hash);
            if (fault == 0) dword(dbx, 16, 0);
            if (fault == 1) dword(dbx, 56, UINT32_MAX);
            if (fault == 2) dword(dbx, 60, UINT32_MAX);
            if (fault == 3) dword(dbx, 64, 0);
            if (fault == 4) dbx.chop(1);
            policy.dbx.insert(ARCH_X86_64, dbx); rufus::BootloaderSecurity info;
            QCOMPARE(rufus::InspectBootloaderSecurity(bytes, policy, true, info), EBADMSG);
        }
        auto policy = policy_; policy.sbat = "BOOTMGRSECURITYVERSIONNUMBER,0x70000";
        QByteArray cycle = svnImage(0x60000); dword(cycle, 1536 + 20, 0x80000000);
        rufus::BootloaderSecurity info;
        QCOMPARE(rufus::InspectBootloaderSecurity(signedImage(cycle), policy, true, info), EBADMSG);
        cancelled = true;
        QCOMPARE(rufus::InspectBootloaderSecurity(bytes, policy_, false, info), ECANCELED);
    }
    void bundledResources()
    {
        const char* paths[] = {"ia32", "x64", "arm", "aarch64", "ia64", "riscv64", "loongarch64"};
        for (int arch = 1; arch <= 7; ++arch) {
            QFile file(QStringLiteral(RUFUS_SOURCE_ROOT "/res/dbx/dbx_") + QString::fromLatin1(paths[arch-1]) + QStringLiteral(".efiauth2"));
            QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(policy_.dbx.value(arch), file.readAll());
        }
    }
};
QTEST_GUILESS_MAIN(TestSecureBoot)
#include "test_secure_boot.moc"
