// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iso_analyze.h"
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

namespace rufus
{
constexpr qsizetype MaximumBootloaderBytes = 64 * 1024 * 1024; // Original PE256Buffer bound.
struct SecureBootPolicy
{
    QHash<int, QByteArray> dbx;
    QList<QByteArray> cachedIssuers; // Bounded DER certificates; metadata lookup only.
    QByteArray sbat, activeCertificates, revokedCertificates, ssp;
};
struct BootloaderSecurity
{
    bool signedImage = false, issuer = false, secureBootAuthority = false;
    int revocation = 0; // Original order: DBX, SSP, SBAT, SVN, expert certificate.
    QString certificateName;
    QByteArray certificateSha1, pe256;
};
// Original Windows metadata classification; no target firmware or host trust
// store, publisher authentication, signature-validity or time-policy claim.
// Return errno; malformed PE/certificate/policy input never publishes a result.
int InspectBootloaderSecurity(const QByteArray& bytes, const SecureBootPolicy& policy,
    bool expert, BootloaderSecurity& result);
int LoadBundledSecureBootPolicy(SecureBootPolicy& policy);
// Remote lists retain the original nonempty <1-KiB bound and schema.
int ValidateSecureBootList(int kind, const QByteArray& bytes); // 0 SBAT, 1/2 thumbprints.
int ValidateSecureBootDbx(const QByteArray& bytes);
int BootloaderPe256(const QByteArray& bytes, QByteArray& digest);
// Retains the selected ISO/UDF view, checks bounded EFI files and clears the
// secure-boot pending bit only when all reported files could be inspected.
int ScanIsoSecureBoot(const ISO_ANALYSIS_SOURCE& source, ISO_ANALYSIS& analysis,
    const SecureBootPolicy& policy, bool expert);
}
