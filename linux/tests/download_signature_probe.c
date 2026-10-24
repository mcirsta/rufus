/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "download_signature.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>

static const char* fault = "none";
static int sha256_selected;
static int padding_selected;

BIGNUM* __real_BN_bin2bn(const unsigned char*, int, BIGNUM*);
BIGNUM* __wrap_BN_bin2bn(const unsigned char* bytes, int size, BIGNUM* number)
{
    if (strcmp(fault, "modulus-allocation") == 0) return NULL;
    return __real_BN_bin2bn(bytes, size, number);
}
OSSL_PARAM_BLD* __real_OSSL_PARAM_BLD_new(void);
OSSL_PARAM_BLD* __wrap_OSSL_PARAM_BLD_new(void)
{
    if (strcmp(fault, "builder-allocation") == 0) return NULL;
    return __real_OSSL_PARAM_BLD_new();
}
OSSL_PARAM* __real_OSSL_PARAM_BLD_to_param(OSSL_PARAM_BLD*);
OSSL_PARAM* __wrap_OSSL_PARAM_BLD_to_param(OSSL_PARAM_BLD* builder)
{
    if (strcmp(fault, "parameter-allocation") == 0) return NULL;
    return __real_OSSL_PARAM_BLD_to_param(builder);
}
int __real_EVP_PKEY_fromdata(EVP_PKEY_CTX*, EVP_PKEY**, int, OSSL_PARAM*);
int __wrap_EVP_PKEY_fromdata(EVP_PKEY_CTX* context, EVP_PKEY** key, int selection, OSSL_PARAM* parameters)
{
    if (strcmp(fault, "key-import") == 0) return 0;
    return __real_EVP_PKEY_fromdata(context, key, selection, parameters);
}
EVP_MD_CTX* __real_EVP_MD_CTX_new(void);
EVP_MD_CTX* __wrap_EVP_MD_CTX_new(void)
{
    if (strcmp(fault, "digest-allocation") == 0) return NULL;
    return __real_EVP_MD_CTX_new();
}
int __real_EVP_DigestVerifyInit(EVP_MD_CTX*, EVP_PKEY_CTX**, const EVP_MD*, ENGINE*, EVP_PKEY*);
int __wrap_EVP_DigestVerifyInit(EVP_MD_CTX* context, EVP_PKEY_CTX** key_context,
    const EVP_MD* digest, ENGINE* engine, EVP_PKEY* key)
{
    sha256_selected = EVP_MD_is_a(digest, "SHA256");
    if (strcmp(fault, "digest-setup") == 0) return 0;
    return __real_EVP_DigestVerifyInit(context, key_context, digest, engine, key);
}
int __real_EVP_PKEY_CTX_set_rsa_padding(EVP_PKEY_CTX*, int);
int __wrap_EVP_PKEY_CTX_set_rsa_padding(EVP_PKEY_CTX* context, int padding)
{
    padding_selected = padding;
    if (strcmp(fault, "padding-setup") == 0) return 0;
    return __real_EVP_PKEY_CTX_set_rsa_padding(context, padding);
}
int __real_EVP_DigestVerify(EVP_MD_CTX*, const unsigned char*, size_t, const unsigned char*, size_t);
int __wrap_EVP_DigestVerify(EVP_MD_CTX* context, const unsigned char* signature, size_t signature_size,
    const unsigned char* data, size_t data_size)
{
    if (strcmp(fault, "verify-error") == 0) return -1;
    if (strcmp(fault, "verify-rejection") == 0) return 0;
    return __real_EVP_DigestVerify(context, signature, signature_size, data, data_size);
}

static int ReadFixture(const char* path, unsigned char** bytes, size_t* size)
{
    const long maximum_fixture_bytes = 1024 * 1024;
    FILE* file = fopen(path, "rb");
    long length;
    int error = EIO;
    if (file == NULL) return errno;
    if (fseek(file, 0, SEEK_END) != 0) goto out;
    length = ftell(file);
    if (length < 0 || length > maximum_fixture_bytes) goto out;
    if (fseek(file, 0, SEEK_SET) != 0) goto out;
    *bytes = malloc(length == 0 ? 1 : (size_t)length);
    if (*bytes == NULL) { error = ENOMEM; goto out; }
    *size = (size_t)length;
    if (fread(*bytes, 1, *size, file) != *size) goto out;
    error = 0;
out:
    fclose(file);
    return error;
}

int main(int argc, char** argv)
{
    unsigned char *data = NULL, *signature = NULL, *data_before = NULL, *signature_before = NULL;
    size_t data_size = 0, signature_size = 0;
    int error;
    int unchanged;
    int status = 1;
    if (argc != 4) return 2;
    error = ReadFixture(argv[1], &data, &data_size);
    if (error != 0) goto out;
    error = ReadFixture(argv[2], &signature, &signature_size);
    if (error != 0) goto out;
    data_before = malloc(data_size == 0 ? 1 : data_size);
    signature_before = malloc(signature_size == 0 ? 1 : signature_size);
    if (data_before == NULL || signature_before == NULL) goto out;
    memcpy(data_before, data, data_size);
    memcpy(signature_before, signature, signature_size);
    fault = argv[3];
    if (strcmp(fault, "missing-provider") == 0 && EVP_set_default_properties(NULL, "provider=missing") != 1)
        goto out;
    // Prefix lookup may make several attempts in one process. A failed attempt
    // must release its objects and not poison the next authentic candidate.
    if (strcmp(fault, "reject-then-valid") == 0)
    {
        if (data_size == 0) goto out;
        data[0] ^= 1;
        error = VerifyRufusDownloadSignature(data, data_size, signature, signature_size);
        data[0] ^= 1;
        if (error != EBADMSG) goto out;
    }
    else if (strcmp(fault, "setup-error-then-valid") == 0)
    {
        fault = "key-import";
        error = VerifyRufusDownloadSignature(data, data_size, signature, signature_size);
        fault = "none";
        if (error != EIO) goto out;
    }
    else if (strcmp(fault, "provider-error-then-valid") == 0)
    {
        if (EVP_set_default_properties(NULL, "provider=missing") != 1) goto out;
        error = VerifyRufusDownloadSignature(data, data_size, signature, signature_size);
        if (EVP_set_default_properties(NULL, NULL) != 1) goto out;
        if (error != EIO) goto out;
    }
    error = VerifyRufusDownloadSignature(strcmp(fault, "null-data") == 0 ? NULL : data, data_size,
        strcmp(fault, "null-signature") == 0 ? NULL : signature, signature_size);
    unchanged = memcmp(data_before, data, data_size) == 0 && memcmp(signature_before, signature, signature_size) == 0;
    printf("ERROR=%d UNCHANGED=%d SHA256=%d PADDING=%d\n", error, unchanged, sha256_selected, padding_selected);
    status = 0;
out:
    free(signature_before);
    free(data_before);
    free(signature);
    free(data);
    return status;
}
