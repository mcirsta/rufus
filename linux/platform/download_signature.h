/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Verify Rufus's raw RSA-2048/PKCS#1 v1.5 SHA-256 signature with its shared,
 * fixed download key. Buffers must be accessible for the supplied lengths;
 * neither is modified. Return 0 only for a valid signature, EBADMSG for a bad
 * signature/length, EINVAL for invalid buffer arguments, ENOMEM for allocation
 * failure and EIO for provider/setup failure. Caller must discard failed data.
 * No files, network, cache installation, privilege or module-ABI selection.
 * Like DownloadSignedFile, do not accept an empty payload. */
int VerifyRufusDownloadSignature(const void* data, size_t data_size,
    const void* signature, size_t signature_size);

#ifdef __cplusplus
}
#endif
