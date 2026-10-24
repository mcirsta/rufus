// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QtEndian>
#include <openssl/pkcs7.h>
#include <openssl/x509v3.h>

namespace secureboot_fixture
{
inline void word(QByteArray& bytes, int offset, quint16 value) { qToLittleEndian(value, bytes.data() + offset); }
inline void dword(QByteArray& bytes, int offset, quint32 value) { qToLittleEndian(value, bytes.data() + offset); }
inline QByteArray peImage(bool x64 = true)
{
    QByteArray bytes(2048, '\0'); bytes.replace(0, 2, "MZ"); dword(bytes, 60, 64);
    bytes.replace(64, 4, QByteArray("PE\0\0", 4)); word(bytes, 68, x64 ? 0x8664 : 0x14c); word(bytes, 70, 3);
    const int optionalSize = x64 ? 240 : 224; word(bytes, 84, quint16(optionalSize));
    word(bytes, 88, x64 ? 0x20b : 0x10b); dword(bytes, 88 + 36, 512); dword(bytes, 88 + 60, 512); word(bytes, 88 + 68, 10);
    dword(bytes, 88 + (x64 ? 108 : 92), 16);
    const QByteArray names[] = {".text", ".sbat", ".rsrc"};
    for (int index = 0; index < 3; ++index) {
        const int entry = 88 + optionalSize + index * 40;
        bytes.replace(entry, names[index].size(), names[index]);
        dword(bytes, entry + 8, 512); dword(bytes, entry + 12, quint32(index + 1) * 4096);
        dword(bytes, entry + 16, 512); dword(bytes, entry + 20, quint32(index + 1) * 512);
    }
    bytes.replace(512, 512, QByteArray(512, 'T'));
    return bytes;
}
inline QByteArray sbatImage(const QByteArray& rows)
{
    QByteArray bytes = peImage(); bytes.replace(1024, rows.size(), rows); return bytes;
}
inline QByteArray svnImage(quint32 version)
{
    QByteArray bytes = peImage(); constexpr int root = 1536;
    dword(bytes, 88 + 112 + 16, 0x3000); dword(bytes, 88 + 112 + 20, 512);
    word(bytes, root + 12, 1); dword(bytes, root + 16, 0x80000080); dword(bytes, root + 20, 0x80000020);
    word(bytes, root + 32 + 14, 1); dword(bytes, root + 48, 1); dword(bytes, root + 52, 0x80000040);
    word(bytes, root + 64 + 14, 1); dword(bytes, root + 80, 1033); dword(bytes, root + 84, 96);
    dword(bytes, root + 96, 0x3100); dword(bytes, root + 100, 4);
    const QByteArray name("BOOTMGRSECURITYVERSIONNUMBER"); word(bytes, root + 128, quint16(name.size()));
    for (int index = 0; index < name.size(); ++index) word(bytes, root + 130 + index * 2, quint8(name[index]));
    dword(bytes, root + 256, version); return bytes;
}
inline QByteArray sha1(X509* certificate)
{
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned size = 0;
    if (X509_digest(certificate, EVP_sha1(), digest, &size) != 1) return {};
    return QByteArray(reinterpret_cast<char*>(digest), size);
}
inline X509* certificate(EVP_PKEY* key, const char* name, X509* issuer, EVP_PKEY* issuerKey)
{
    X509* value = X509_new(); if (!value) return nullptr;
    X509_set_version(value, 2); ASN1_INTEGER_set(X509_get_serialNumber(value), issuer ? 2 : 1);
    X509_gmtime_adj(X509_getm_notBefore(value), -86400); X509_gmtime_adj(X509_getm_notAfter(value), 86400);
    X509_set_pubkey(value, key);
    X509_NAME* subject = X509_get_subject_name(value);
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(name), -1, -1, 0);
    X509_set_issuer_name(value, issuer ? X509_get_subject_name(issuer) : subject);
    X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, nullptr, NID_basic_constraints,
        const_cast<char*>(issuer ? "critical,CA:FALSE" : "critical,CA:TRUE"));
    if (!extension || X509_add_ext(value, extension, -1) != 1 || X509_sign(value, issuerKey ? issuerKey : key, EVP_sha256()) <= 0) {
        X509_EXTENSION_free(extension); X509_free(value); return nullptr;
    }
    X509_EXTENSION_free(extension); return value;
}
inline QByteArray dbxList(const QByteArray& hash)
{
    QByteArray bytes(40 + 28 + 48, '\0'); dword(bytes, 16, 24);
    bytes.replace(40, 16, QByteArray::fromHex("2616c4c14c509240aca941f936934328"));
    dword(bytes, 56, 76); dword(bytes, 64, 48); bytes.replace(84, 32, hash); return bytes;
}

inline QByteArray signedBootloader(const QByteArray& input)
{
    EVP_PKEY* rootKey = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", size_t(2048));
    EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", size_t(2048));
    X509* root = rootKey ? certificate(rootKey, "Synthetic UEFI CA", nullptr, nullptr) : nullptr;
    X509* leaf = key && root ? certificate(key, "Synthetic EFI signer", root, rootKey) : nullptr;
    STACK_OF(X509)* chain = sk_X509_new_null();
    BIO* content = BIO_new_mem_buf("fixture", 7);
    PKCS7* message = nullptr;
    if (leaf && chain && content && sk_X509_push(chain, root) > 0)
        message = PKCS7_sign(leaf, key, chain, content, PKCS7_BINARY);
    unsigned char* encoded = nullptr;
    const int size = message ? i2d_PKCS7(message, &encoded) : 0;
    QByteArray bytes;
    if (size > 0) {
        QByteArray cert(8, '\0'); cert.append(reinterpret_cast<const char*>(encoded), size);
        dword(cert, 0, quint32(cert.size())); word(cert, 4, 0x200); word(cert, 6, 2);
        while (cert.size() % 8) cert.append('\0');
        bytes = input;
        const int table = 88 + (qFromLittleEndian<quint16>(bytes.constData() + 88) == 0x20b ? 112 : 96) + 32;
        dword(bytes, table, quint32(bytes.size())); dword(bytes, table + 4, quint32(cert.size())); bytes.append(cert);
    }
    OPENSSL_free(encoded); PKCS7_free(message); BIO_free(content); sk_X509_free(chain);
    X509_free(leaf); X509_free(root); EVP_PKEY_free(key); EVP_PKEY_free(rootKey);
    return bytes;
}
}
