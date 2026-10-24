/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ffu_catalog.h"
#include "ffu_production_root.inc"
#include "rufus_reporting.h"
#include <errno.h>
#include <string.h>
#include <openssl/asn1t.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pkcs7.h>
#include <openssl/x509v3.h>

/* CTL_INFO and CTL_ENTRY wire fields, not a restriction on catalog members. */
typedef struct { ASN1_OCTET_STRING* subject; STACK_OF(X509_ATTRIBUTE)* attributes; } FFU_CTL_ENTRY;
ASN1_SEQUENCE(FFU_CTL_ENTRY) = {
    ASN1_SIMPLE(FFU_CTL_ENTRY, subject, ASN1_OCTET_STRING),
    ASN1_SET_OF(FFU_CTL_ENTRY, attributes, X509_ATTRIBUTE)
} ASN1_SEQUENCE_END(FFU_CTL_ENTRY)
IMPLEMENT_ASN1_FUNCTIONS(FFU_CTL_ENTRY)
DEFINE_STACK_OF(FFU_CTL_ENTRY)

typedef struct {
    ASN1_INTEGER* version;
    STACK_OF(ASN1_OBJECT)* usage;
    ASN1_OCTET_STRING* identifier;
    ASN1_INTEGER* sequence;
    ASN1_TIME* updated;
    ASN1_TIME* next;
    X509_ALGOR* algorithm;
    STACK_OF(FFU_CTL_ENTRY)* entries;
    STACK_OF(X509_EXTENSION)* extensions;
} FFU_CTL;
ASN1_SEQUENCE(FFU_CTL) = {
    ASN1_OPT(FFU_CTL, version, ASN1_INTEGER),
    ASN1_SEQUENCE_OF(FFU_CTL, usage, ASN1_OBJECT),
    ASN1_OPT(FFU_CTL, identifier, ASN1_OCTET_STRING),
    ASN1_OPT(FFU_CTL, sequence, ASN1_INTEGER),
    ASN1_SIMPLE(FFU_CTL, updated, ASN1_TIME),
    ASN1_OPT(FFU_CTL, next, ASN1_TIME),
    ASN1_SIMPLE(FFU_CTL, algorithm, X509_ALGOR),
    ASN1_SEQUENCE_OF_OPT(FFU_CTL, entries, FFU_CTL_ENTRY),
    ASN1_EXP_SEQUENCE_OF_OPT(FFU_CTL, extensions, X509_EXTENSION, 0)
} ASN1_SEQUENCE_END(FFU_CTL)
IMPLEMENT_ASN1_FUNCTIONS(FFU_CTL)

static int CatalogContent(PKCS7* message, const uint8_t** content, long* length)
{
    PKCS7* inner = message->d.sign->contents;
    char oid[64];
    if (inner == NULL || OBJ_obj2txt(oid, sizeof(oid), inner->type, 1) <= 0) return EBADMSG;
    if (strcmp(oid, "1.3.6.1.4.1.311.10.1") != 0) return ENOTSUP;
    const ASN1_TYPE* value = inner->d.other;
    if (value == NULL || value->type != V_ASN1_SEQUENCE || value->value.sequence == NULL) return EBADMSG;
    const ASN1_STRING* sequence = value->value.sequence;
    const uint8_t* begin = ASN1_STRING_get0_data(sequence);
    const long bytes = ASN1_STRING_length(sequence);
    if (begin == NULL || bytes <= 0) return EBADMSG;
    const uint8_t* position = begin;
    FFU_CTL* ctl = d2i_FFU_CTL(NULL, &position, bytes);
    const bool valid = ctl != NULL && position == begin + bytes
        && ctl->usage != NULL && ASN1_TIME_check(ctl->updated) == 1;
    FFU_CTL_free(ctl);
    if (!valid) return EBADMSG;
    position = begin;
    int tag, type;
    const int flags = ASN1_get_object(&position, length, &tag, &type, bytes);
    if (flags != V_ASN1_CONSTRUCTED || tag != V_ASN1_SEQUENCE || type != V_ASN1_UNIVERSAL
        || *length < 0 || position + *length != begin + bytes) return EBADMSG;
    /* Authenticode signs the sequence contents, excluding its DER header.
     * Supplying the bytes explicitly also supports OpenSSL before 3.5. */
    *content = position;
    return 0;
}

static int VerifySigner(PKCS7* message, int selected, const uint8_t* content, long bytes)
{
    PKCS7* copy = PKCS7_dup(message);
    if (copy == NULL) return ENOMEM;
    STACK_OF(PKCS7_SIGNER_INFO)* signers = PKCS7_get_signer_info(copy);
    /* PKCS7_dup DER-sorts the signer SET. Copy the selected original signer
     * explicitly, so the verified signature and inspected certificate agree. */
    PKCS7_SIGNER_INFO* original = sk_PKCS7_SIGNER_INFO_value(PKCS7_get_signer_info(message), selected);
    PKCS7_SIGNER_INFO* chosen = ASN1_item_dup(ASN1_ITEM_rptr(PKCS7_SIGNER_INFO), original);
    if (chosen == NULL) { PKCS7_free(copy); return ENOMEM; }
    while (sk_PKCS7_SIGNER_INFO_num(signers) != 0) PKCS7_SIGNER_INFO_free(sk_PKCS7_SIGNER_INFO_pop(signers));
    if (sk_PKCS7_SIGNER_INFO_push(signers, chosen) == 0) {
        PKCS7_SIGNER_INFO_free(chosen);
        PKCS7_free(copy);
        return ENOMEM;
    }
    BIO* input = BIO_new_mem_buf(content, (int)bytes);
    int error = input == NULL ? ENOMEM : EKEYREJECTED;
    if (input != NULL && PKCS7_verify(copy, NULL, NULL, input, NULL, PKCS7_NOVERIFY | PKCS7_BINARY) == 1)
        error = 0;
    BIO_free(input);
    PKCS7_free(copy);
    ERR_clear_error();
    return error;
}

static int ProductionChain(PKCS7* message, X509* signer, bool* production)
{
    static const uint8_t thumbprints[2][20] = {
        {0x3b,0x1e,0xfd,0x3a,0x66,0xea,0x28,0xb1,0x66,0x97,0x39,0x47,0x03,0xa7,0x2c,0xa3,0x40,0xa0,0x5b,0xd5},
        {0x05,0x86,0x1f,0xde,0x0c,0xca,0xcd,0x6e,0xec,0x8d,0x91,0xdb,0x6e,0x0f,0x22,0xc2,0x57,0x74,0x85,0x32}
    };
    const uint8_t* root_bytes = ffuProductionRoot;
    X509* root = d2i_X509(NULL, &root_bytes, sizeof(ffuProductionRoot));
    X509_STORE* store = X509_STORE_new();
    X509_STORE_CTX* context = X509_STORE_CTX_new();
    STACK_OF(X509)* chain = NULL;
    int error = ENOMEM;
    *production = false;
    if (root == NULL || store == NULL || context == NULL) goto out;
    error = EIO;
    if (X509_STORE_add_cert(store, root) != 1
        || X509_STORE_CTX_init(context, store, signer, message->d.sign->cert) != 1) goto out;
    X509_VERIFY_PARAM* parameters = X509_STORE_CTX_get0_param(context);
    X509_VERIFY_PARAM_set_flags(parameters, X509_V_FLAG_NO_CHECK_TIME | X509_V_FLAG_PARTIAL_CHAIN);
    X509_VERIFY_PARAM_set_depth(parameters, 100);
    /* Windows inspects the built chain, including untrusted/expired test roots;
     * its general trust status does not decide whether a test signer is allowed. */
    (void)X509_verify_cert(context);
    chain = X509_STORE_CTX_get1_chain(context);
    if (chain == NULL) goto out;
    for (int index = 0; index < sk_X509_num(chain); index++) {
        if (IsRufusJobCancelled()) { error = ECANCELED; goto out; }
        uint8_t digest[EVP_MAX_MD_SIZE];
        unsigned size = 0;
        if (X509_digest(sk_X509_value(chain, index), EVP_sha1(), digest, &size) != 1 || size != 20) goto out;
        if (memcmp(digest, thumbprints[0], 20) == 0 || memcmp(digest, thumbprints[1], 20) == 0) *production = true;
    }
    error = 0;
out:
    sk_X509_pop_free(chain, X509_free);
    X509_STORE_CTX_free(context);
    X509_STORE_free(store);
    X509_free(root);
    ERR_clear_error();
    return error;
}

int VerifyFfuCatalog(const void* bytes, size_t size, FFU_CATALOG_RESULT* result)
{
    if (result == NULL) return EINVAL;
    memset(result, 0, sizeof(*result));
    if (size == 0) return 0;
    if (bytes == NULL) return EINVAL;
    if (size > FFU_MAX_CATALOG_BYTES) return ENOTSUP;
    if (IsRufusJobCancelled()) return ECANCELED;
    const uint8_t* position = bytes;
    PKCS7* message = d2i_PKCS7(NULL, &position, (long)size);
    int error = EBADMSG;
    FFU_CATALOG_RESULT completed = {0};
    if (message == NULL || position != (const uint8_t*)bytes + size || !PKCS7_type_is_signed(message)
        || message->d.sign == NULL) goto out;
    const uint8_t* content;
    long length;
    error = CatalogContent(message, &content, &length);
    if (error != 0) goto out;
    STACK_OF(PKCS7_SIGNER_INFO)* signers = PKCS7_get_signer_info(message);
    const int count = sk_PKCS7_SIGNER_INFO_num(signers);
    if (count < 0) { error = EBADMSG; goto out; }
    if (count != 0) {
        error = EKEYREJECTED;
        for (int index = 0; index < count; index++) {
            if (IsRufusJobCancelled()) { error = ECANCELED; goto out; }
            error = VerifySigner(message, index, content, length);
            if (error == ENOMEM) goto out;
            if (error != 0) continue;
            X509* signer = PKCS7_cert_from_signer_info(message, sk_PKCS7_SIGNER_INFO_value(signers, index));
            bool production = false;
            error = signer == NULL ? EKEYREJECTED : ProductionChain(message, signer, &production);
            if (error == 0 && production) error = ENOTSUP;
            if (error != 0) goto out;
            completed.signed_catalog = true;
            break;
        }
        if (error != 0) goto out;
    }
    unsigned digest_bytes = 0;
    if (EVP_Digest(bytes, size, completed.sha256, &digest_bytes, EVP_sha256(), NULL) != 1 || digest_bytes != 32) {
        error = EIO; goto out;
    }
    *result = completed;
    error = 0;
out:
    PKCS7_free(message);
    ERR_clear_error();
    return error;
}
