// SPDX-License-Identifier: GPL-3.0-or-later
#include "application_updates.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QTimeZone>
#include <cerrno>
#include <limits>

namespace rufus
{
namespace
{
constexpr qsizetype maximumReleasePageBytes = 4 * 1024 * 1024;
constexpr int releasesPerPage = 100;
constexpr int maximumReleasePages = 5;
constexpr qsizetype maximumReleaseNotesCharacters = 32768;

bool repositoryName(const QString& name)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$"));
    if (name.size() > 200) return false;
    for (const QString& part : name.split(QLatin1Char('/')))
        if (part == QLatin1String(".") || part == QLatin1String("..")) return false;
    return pattern.match(name).hasMatch();
}

QString assetVersion(const QString& name, const QString& architecture)
{
    const QString numbers = QStringLiteral("([0-9]+\\.[0-9]+\\.[0-9]+)");
    const QString arch = QRegularExpression::escape(architecture);
    const QRegularExpression archive(QStringLiteral("^rufus-linux-preview-") + numbers
        + QStringLiteral("(-linux-preview\\.[0-9]+)?-") + arch + QStringLiteral("\\.tar\\.gz$"));
    auto match = archive.match(name);
    if (match.hasMatch()) return match.captured(1) + match.captured(2);
    const QRegularExpression package(QStringLiteral("^rufus-linux-preview-") + numbers
        + QStringLiteral("(?:\\.preview([0-9]+))?-[1-9][0-9]*-") + arch + QStringLiteral("\\.pkg\\.tar\\.zst$"));
    match = package.match(name);
    if (!match.hasMatch()) return {};
    return match.captured(1) + (match.captured(2).isEmpty() ? QString() : QStringLiteral("-linux-preview.") + match.captured(2));
}
}

bool ParseApplicationVersion(const QString& text, quint16 (&upstream)[3], quint32& preview)
{
    if (text.size() > 64) return false;
    static const QRegularExpression pattern(QStringLiteral("^([0-9]+)\\.([0-9]+)\\.([0-9]+)(?:-linux-preview\\.([0-9]+))?$"));
    const auto match = pattern.match(text);
    if (!match.hasMatch()) return false;
    quint16 words[3] = {};
    for (int index = 0; index < 3; ++index) {
        bool valid = false; const uint number = match.captured(index + 1).toUInt(&valid);
        if (!valid || number > std::numeric_limits<quint16>::max()) return false;
        words[index] = quint16(number);
    }
    quint32 revision = std::numeric_limits<quint32>::max(); // Stable follows every preview.
    if (!match.captured(4).isEmpty()) {
        bool valid = false; revision = match.captured(4).toUInt(&valid);
        if (!valid || revision == std::numeric_limits<quint32>::max()) return false;
    }
    for (int index = 0; index < 3; ++index) upstream[index] = words[index];
    preview = revision; return true;
}

int CompareApplicationVersions(const QString& left, const QString& right)
{
    quint16 a[3], b[3]; quint32 ap = 0, bp = 0;
    if (!ParseApplicationVersion(left, a, ap) || !ParseApplicationVersion(right, b, bp)) return 0;
    for (int index = 0; index < 3; ++index) if (a[index] != b[index]) return a[index] < b[index] ? -1 : 1;
    return ap == bp ? 0 : ap < bp ? -1 : 1;
}

bool ParseApplicationReleasePage(const QByteArray& data, const QString& repository,
    const QString& architecture, bool includeBetas, ApplicationRelease& newest, int& rows, QString& problem)
{
    problem.clear(); rows = 0;
    if (data.isEmpty() || data.size() > maximumReleasePageBytes || !repositoryName(repository) || architecture.isEmpty()) {
        problem = QStringLiteral("Invalid native release catalog bounds or repository."); return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        problem = QStringLiteral("Invalid native release catalog JSON."); return false;
    }
    const auto releases = document.array();
    if (releases.size() > releasesPerPage) { problem = QStringLiteral("Too many releases on one page."); return false; }
    ApplicationRelease candidate = newest;
    for (const auto& value : releases) {
        if (!value.isObject()) { problem = QStringLiteral("Invalid release entry."); return false; }
        const auto release = value.toObject();
        if (!release.value(QStringLiteral("draft")).isBool() || !release.value(QStringLiteral("prerelease")).isBool()) {
            problem = QStringLiteral("Missing release channel flags."); return false;
        }
        const bool excluded = release.value(QStringLiteral("draft")).toBool()
            || (release.value(QStringLiteral("prerelease")).toBool() && !includeBetas);
        if (excluded) continue;
        const auto assetsValue = release.value(QStringLiteral("assets"));
        if (!assetsValue.isArray() || assetsValue.toArray().size() > 128) {
            problem = QStringLiteral("Invalid native release assets."); return false;
        }
        QString version;
        for (const auto& assetValue : assetsValue.toArray()) {
            if (!assetValue.isObject()) { problem = QStringLiteral("Invalid native asset entry."); return false; }
            const auto asset = assetValue.toObject();
            const QString parsed = assetVersion(asset.value(QStringLiteral("name")).toString(), architecture);
            if (parsed.isEmpty()) continue; // Windows binaries and other build architectures are not native updates.
            quint16 upstream[3]; quint32 preview = 0;
            const double bytes = asset.value(QStringLiteral("size")).toDouble(-1);
            const bool uploaded = asset.value(QStringLiteral("state")).toString() == QLatin1String("uploaded");
            const bool bounded = bytes > 0 && bytes <= 16.0 * 1024 * 1024 * 1024 && bytes == double(quint64(bytes));
            if (!uploaded || !bounded || !ParseApplicationVersion(parsed, upstream, preview)) {
                problem = QStringLiteral("Invalid native release version or asset metadata."); return false;
            }
            if (!version.isEmpty() && CompareApplicationVersions(parsed, version) != 0) {
                problem = QStringLiteral("Conflicting native versions within one release."); return false;
            }
            version = parsed;
        }
        if (version.isEmpty()) continue;
        const QString tag = release.value(QStringLiteral("tag_name")).toString();
        const auto body = release.value(QStringLiteral("body"));
        const QString notes = body.isNull() ? QString() : body.toString();
        const bool validText = !tag.isEmpty() && tag.size() <= 128 && !tag.contains(QChar::Null)
            && (body.isNull() || body.isString()) && notes.size() <= maximumReleaseNotesCharacters;
        if (!validText) { problem = QStringLiteral("Invalid native release description."); return false; }
        QUrl expected(QStringLiteral("https://github.com"));
        expected.setPath(QLatin1Char('/') + repository + QStringLiteral("/releases/tag/") + tag);
        const QUrl page(release.value(QStringLiteral("html_url")).toString(), QUrl::StrictMode);
        if (!page.isValid() || page != expected) { problem = QStringLiteral("Unexpected native release page."); return false; }
        if (candidate.version.isEmpty() || CompareApplicationVersions(version, candidate.version) > 0)
            candidate = {version, notes, page};
    }
    newest = candidate; rows = int(releases.size()); return true;
}

bool ApplicationUpdateDue(int interval, qint64 last, qint64 now)
{
    if (interval <= 0 || now <= 0) return false;
    if (last <= 0 || last > now) return true; // Recover after a clock is corrected backwards.
    return now - last >= interval;
}

ApplicationUpdates::ApplicationUpdates(QObject* parent, const ApplicationUpdateSources& sources)
    : QObject(parent), download_(this), sources_(sources)
{
    qRegisterMetaType<ApplicationUpdateResult>();
    connect(&download_, &HttpDownload::completed, this, &ApplicationUpdates::downloaded);
}

int ApplicationUpdates::start(const QString& current, const QString& architecture, bool includeBetas, bool force, int timeoutMs)
{
    if (active_) return EBUSY;
    quint16 upstream[3]; quint32 preview = 0;
    const bool http = sources_.api.scheme() == QLatin1String("https") || sources_.api.scheme() == QLatin1String("http");
    const bool endpoint = sources_.api.isValid() && http && !sources_.api.host().isEmpty() && sources_.api.userInfo().isEmpty();
    if (!ParseApplicationVersion(current, upstream, preview) || architecture.isEmpty() || !repositoryName(sources_.repository)
        || !endpoint || timeoutMs <= 0 || sources_.api.hasQuery() || sources_.api.hasFragment()) return EINVAL;
    current_ = current; architecture_ = architecture; includeBetas_ = includeBetas; force_ = force;
    if (verbose_ > 1) emit logMessage(QStringLiteral("Native update check: current=%1 architecture=%2 betas=%3 force=%4")
        .arg(current, architecture).arg(includeBetas).arg(force));
    newest_ = {}; page_ = 0; serverTime_ = 0; timeoutMs_ = timeoutMs; active_ = true;
    requestPage(); return 0;
}

void ApplicationUpdates::requestPage()
{
    QUrl url = sources_.api;
    url.setPath(QStringLiteral("/repos/") + sources_.repository + QStringLiteral("/releases"));
    QUrlQuery query; query.addQueryItem(QStringLiteral("per_page"), QString::number(releasesPerPage));
    query.addQueryItem(QStringLiteral("page"), QString::number(++page_)); url.setQuery(query);
    if (verbose_) emit logMessage(QStringLiteral("Native update request: %1").arg(url.toDisplayString()));
    const int error = download_.start(url, maximumReleasePageBytes, timeoutMs_, false, QByteArray("application/vnd.github+json"));
    if (error != 0) finish(error);
}

void ApplicationUpdates::downloaded(HttpDownloadResult result)
{
    if (!active_) return;
    if (result.error != 0) { finish(result.error); return; }
    // HTTP uses IMF-fixdate with a literal GMT zone; Qt's RFC2822 reader
    // requires a numeric offset and does not accept this HTTP spelling.
    const auto parsed = QLocale::c().toDateTime(QString::fromLatin1(result.date), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
    const QDateTime date(parsed.date(), parsed.time(), QTimeZone(0));
    const bool validDate = date.isValid() && date.toSecsSinceEpoch() > 0;
    if (!validDate) { finish(EPROTO); return; }
    int rows = 0; QString problem;
    if (!ParseApplicationReleasePage(result.bytes, sources_.repository, architecture_, includeBetas_, newest_, rows, problem)) {
        finish(EBADMSG); return;
    }
    serverTime_ = date.toSecsSinceEpoch();
    if (verbose_ > 1) emit logMessage(QStringLiteral("Native update response: HTTP=%1 bytes=%2 rows=%3 validDate=%4")
        .arg(result.httpStatus).arg(result.bytes.size()).arg(rows).arg(validDate));
    if (rows == releasesPerPage) {
        if (page_ == maximumReleasePages) { finish(E2BIG); return; }
        requestPage(); return;
    }
    finish(0);
}

void ApplicationUpdates::cancel() { if (active_) download_.cancel(); }
void ApplicationUpdates::finish(int error)
{
    if (!active_) return;
    active_ = false;
    if (verbose_) emit logMessage(QStringLiteral("Native update result: error=%1 newest=%2 timestamp=%3")
        .arg(error).arg(newest_.version).arg(serverTime_));
    ApplicationUpdateResult result; result.error = error;
    if (error == 0) {
        result.release = newest_; result.releaseFound = !newest_.version.isEmpty(); result.serverTime = serverTime_;
        result.available = result.releaseFound && (force_ || CompareApplicationVersions(newest_.version, current_) > 0);
    }
    emit completed(result);
}
}
