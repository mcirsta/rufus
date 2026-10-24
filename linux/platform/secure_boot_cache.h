// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "secure_boot.h"
#include <QUrl>

namespace rufus
{
struct DbxSource { int architecture; QUrl url; quint64 timestamp; };
QList<DbxSource> BundledDbxSources();
QUrl DbxCommitUrl(const QUrl& content);
quint64 DbxCommitTimestamp(const QByteArray& json);
int ReadSecureBootDbxCache(const QString& directory, int architecture, QByteArray& bytes, quint64& timestamp);
int StoreSecureBootDbxCache(const QString& directory, int architecture, const QByteArray& bytes, quint64 timestamp);
int LoadCachedSecureBootPolicy(const QString& directory, SecureBootPolicy& policy);
}
