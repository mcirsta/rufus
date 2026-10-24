// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QStringList>
#include <QVector>

namespace rufus
{
struct ApplicationLocale
{
    QString tag, name;
    QVector<unsigned> lcids;
    unsigned major = 0, minor = 0;
    bool rightToLeft = false;
    struct Entry { QString key, value; };
    QVector<Entry> entries;
};

class LocaleCatalog
{
public:
    bool load(const QByteArray& data, QString& problem);
    const QVector<ApplicationLocale>& locales() const { return locales_; }
    int selection(const QString& choice) const;
    QString text(int locale, const QString& key) const;
    QString translate(int locale, const QString& source) const;
    void useSourceTexts(const LocaleCatalog& original);
private:
    QString translatedText(int locale, const QString& key) const;
    bool resolve(int locale, QVector<int>& stack, QHash<QString, QString>& texts, int& remaining) const;
    QVector<ApplicationLocale> locales_;
    QVector<QHash<QString, QString>> texts_;
    QHash<QString, QStringList> sourceKeys_;
    QHash<QString, QString> sourceTexts_;
};

// Reads only explicit settings/arguments. English is the initial choice.
bool InitializeApplicationLanguage(const QString& executable, const QString& requested, QString& problem);
const LocaleCatalog& ApplicationLanguages();
QString SelectedApplicationLanguage();
bool SetApplicationLanguage(const QString& tag, QString& problem);
}
