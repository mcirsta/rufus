// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>
namespace rufus
{
struct DownloadEdition { QString name; QList<int> ids; };
struct DownloadRelease { QString name; QList<DownloadEdition> editions; };
struct DownloadProduct { QString name, id; QList<DownloadRelease> releases; };
struct DownloadCatalog
{
    QString version, organization, profile, instance;
    QList<DownloadProduct> products;
};
// Parse original signed Fido data as bounded literals, never execute PowerShell.
int ParseDownloadManifest(const QByteArray& bytes,QUrl& provider);
int DecodeDownloadProvider(const QByteArray& compressed,QByteArray& script);
int ParseDownloadCatalog(const QByteArray& script,DownloadCatalog& catalog);
}
