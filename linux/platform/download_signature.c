/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "download_signature.h"
#include "download_key.h"
#include <errno.h>
#include <stdbool.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>

static int ImportDownloadKey(EVP_PKEY** key)
{
    BIGNUM* modulus = BN_bin2bn(rsa_pubkey_modulus, sizeof(rsa_pubkey_modulus), NULL);
    OSSL_PARAM_BLD* builder = OSSL_PARAM_BLD_new();
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    OSSL_PARAM* parameters = NULL;
    int error = EIO;
    const bool allocated = modulus != NULL && builder != NULL;

    if (!allocated)
    {
        error = ENOMEM;
        goto out;
    }
    if (context == NULL)
        goto out;
    if (OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, modulus) != 1)
        goto out;
    if (OSSL_PARAM_BLD_push_ulong(builder, OSSL_PKEY_PARAM_RSA_E, RUFUS_DOWNLOAD_PUBLIC_EXPONENT) != 1)
        goto out;
    parameters = OSSL_PARAM_BLD_to_param(builder);
    if (parameters == NULL)
    {
        error = ENOMEM;
        goto out;
    }
    if (EVP_PKEY_fromdata_init(context) != 1)
        goto out;
    if (EVP_PKEY_fromdata(context, key, EVP_PKEY_PUBLIC_KEY, parameters) != 1)
        goto out;
    error = 0;
out:
    OSSL_PARAM_free(parameters);
    EVP_PKEY_CTX_free(context);
    OSSL_PARAM_BLD_free(builder);
    BN_free(modulus);
    return error;
}

int VerifyRufusDownloadSignature(const void* data, size_t data_size,
    const void* signature, size_t signature_size)
{
    EVP_PKEY* key = NULL;
    EVP_MD_CTX* context = NULL;
    EVP_PKEY_CTX* verification = NULL;
    int result;
    int error;

    if (data == NULL || signature == NULL)
        return EINVAL;
    if (data_size == 0)
        return EINVAL;
    if (signature_size != RUFUS_DOWNLOAD_SIGNATURE_BYTES)
        return EBADMSG;
    error = ImportDownloadKey(&key);
    if (error != 0)
        goto out;
    context = EVP_MD_CTX_new();
    if (context == NULL)
    {
        error = ENOMEM;
        goto out;
    }
    error = EIO;
    if (EVP_DigestVerifyInit(context, &verification, EVP_sha256(), NULL, key) != 1)
        goto out;
    // Select Rufus's format explicitly; never retry with another hash/padding.
    if (EVP_PKEY_CTX_set_rsa_padding(verification, RSA_PKCS1_PADDING) != 1)
        goto out;
    // Server bytes are already in OpenSSL order. CryptoAPI alone needs reversal.
    result = EVP_DigestVerify(context, signature, signature_size, data, data_size);
    if (result == 1)
        error = 0;
    else if (result == 0)
        error = EBADMSG;
out:
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return error;
}
