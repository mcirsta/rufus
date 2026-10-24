/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { FFU_MAX_CATALOG_BYTES = 16 * 1024 * 1024 };
typedef struct { bool signed_catalog; uint8_t sha256[32]; } FFU_CATALOG_RESULT;

/* Windows desktop FFU policy: accept an unsigned CTL or the first valid signer,
 * and refuse its known production chain. Signature verification does not imply
 * publisher trust: test/self-signed certificates are accepted, as on Windows.
 * No host trust store, network, time or revocation policy is substituted.
 * An absent catalog retains the legacy unsigned-container route. Return errno,
 * clearing result on failure. The caller checks the source and every FFU chunk. */
int VerifyFfuCatalog(const void* bytes, size_t size, FFU_CATALOG_RESULT* result);

#ifdef __cplusplus
}
#endif
